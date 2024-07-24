// NOTE: libc requires this for RTLD_NEXT.
#define _GNU_SOURCE 1

#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/epoll.h>
#include <demi/libos.h>
#include <demi/wait.h>
#include <demi/types.h>
#include <pthread.h>

#include <event2/util.h>
#include <event2/event.h>

#include "event-internal.h"
#include "evthread-internal.h"
#include "evmap-internal.h"
#include "changelist-internal.h"

#ifdef EVENT__HAVE_DEMIEVENT

#define EVENT_BASE_FLAG_DEMIEVENT_USE_CHANGELIST 1
#define QTOKEN_MAX 65535
#define MAX_EVENTS 16

static void *
demievent_init(struct event_base *base);

// Add event
static int
demievent_changelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);
static int
demievent_nochangelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

// Delete event
static int
demievent_changelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);
static int
demievent_nochangelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

// Dispatch
static int
demievent_dispatch(struct event_base *base, struct timeval *);

// Other
static void
demievent_dealloc(struct event_base *base);

struct demivent {
  int fd;
  int type;
  short events;
  demi_qtoken_t qt;
  int next;
  int prev;
};

struct demieventop {
  /* Number of regular epoll events added */
  int n_epoll;
  /* Epoll holding regular events */
  int epfd;

  /* Number of demikernel epoll events */
  int n_demi_epoll;
  /* Epoll holding demikernel events*/
  int demi_epfd;

  /* Libc functions to be used with epoll */
  int (*libc_epoll_create1)(int);
  int (*libc_epoll_ctl)(int, int, int, struct epoll_event *);
  int (*libc_epoll_wait)(int, struct epoll_event *, int, int);

};

static const struct eventop demieventops_changelist = {
  "demievent",
  demievent_init,
  demievent_changelist_add,
  demievent_changelist_del,
  demievent_dispatch,
  demievent_dealloc,
  1, /* need reinit. not sure if this is necessary */
  EV_FEATURE_ET|EV_FEATURE_O1,
  EVENT_CHANGELIST_FDINFO_SIZE
};

const struct eventop demieventops = {
  "demievent",
  demievent_init,
  demievent_nochangelist_add,
  demievent_nochangelist_del,
  demievent_dispatch,
  demievent_dealloc,
  1,
  EV_FEATURE_ET|EV_FEATURE_O1,
  0
};

static void *
demievent_init(struct event_base *base)
{
  int epfd;
  struct demieventop *demiop;

  demiop = mm_calloc(1, sizeof(struct demieventop));
  if (!demiop) {
    fprintf(stderr, "demievent_init: return error\n");
    return NULL;
  }

  assert((demiop->libc_epoll_create1 = dlsym(RTLD_NEXT, "epoll_create1")) != NULL);
  assert((demiop->libc_epoll_ctl = dlsym(RTLD_NEXT, "epoll_ctl")) != NULL);
  assert((demiop->libc_epoll_wait = dlsym(RTLD_NEXT, "epoll_wait")) != NULL);

  epfd = demiop->libc_epoll_create1(0);
  if (epfd == -1) {
    fprintf(stderr, "demievent_init: libc epoll_create failed\n");
    free(demiop);
    return NULL;
  }
  demiop->epfd = epfd;

  epfd = epoll_create1(0);
  if (epfd == -1) {
    fprintf(stderr, "demievent_init: demikernel epoll_create failed\n");
    free(demiop);
    return NULL;
  }
  demiop->demi_epfd = epfd;

  demiop->n_epoll = 0;
  demiop->n_demi_epoll = 0;

  // DEMIDO: Figure out if enabling changelist is better
  if (base->flags & EVENT_BASE_FLAG_DEMIEVENT_USE_CHANGELIST) {
    base->evsel = &demieventops_changelist;
  }

  base->evsel = &demieventops;

  // ret = demi_init(argc, args);
  // if (ret != 0 && ret != EEXIST) {
  //   free(demiop);
  //   return NULL;
  // }

  return (demiop);
}

static int
demievent_changelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  abort();
  return 0;
}

static int
demievent_nochangelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  struct epoll_event event;
  struct demieventop *evbase;

  evbase = (struct demieventop *) base->evbase;

  event.events = EPOLLIN;
  event.data.fd = fd;

  if (fd < 500) {
    evbase->libc_epoll_ctl(evbase->epfd, EPOLL_CTL_ADD, fd, &event);
  } else {
    epoll_ctl(evbase->demi_epfd, EPOLL_CTL_ADD, fd, &event);
  }

  if (fd < 500)
    evbase->n_epoll++;
  else
    evbase->n_demi_epoll++;

  return 0;
}

static int
demievent_changelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  abort();
  return 0;
}

static int
demievent_nochangelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  int ret;
  struct demieventop *evbase;

  evbase = (struct demieventop *) base->evbase;

  if (fd < 500) {
    ret = evbase->libc_epoll_ctl(evbase->epfd, EPOLL_CTL_DEL, fd, NULL);
  } else {
    ret = epoll_ctl(evbase->epfd, EPOLL_CTL_DEL, fd, NULL);
  }

  if (ret == -1 && errno == EBADF) {
    return -1;
  } else if (ret == -1) {
    return -1;
  }

  return 0;
}

static int
demievent_dispatch(struct event_base *base, struct timeval *tv)
{
  int res, ev, i;
  struct demieventop *evbase;
  struct epoll_event events[MAX_EVENTS];
  struct epoll_event demi_events[MAX_EVENTS];
  long timeout = -1;

  evbase = (struct demieventop *) base->evbase;
  ev = 0;

  if (tv != NULL) {
    timeout = evutil_tv_to_msec_(tv);
  }

  // Dispatch demikernel events
  if (evbase->n_demi_epoll > 0) {
    res = epoll_wait(evbase->demi_epfd, demi_events, MAX_EVENTS, timeout);

    for (i = 0; i < res; i++) {
      if ((demi_events[i].events & EPOLLHUP) &&
          !(demi_events[i].events & EPOLLRDHUP)) {
        ev = EV_READ | EV_WRITE;
      } else {
        if (demi_events[i].events & EPOLLIN)
          ev |= EV_READ;
        if (demi_events[i].events & EPOLLOUT)
          ev |= EV_WRITE;
        if (demi_events[i].events & EPOLLRDHUP)
          ev |= EV_CLOSED;
      }

      evmap_io_active_(base, demi_events[i].data.fd, ev);
    }
  }


  // Dispatch regular epoll events
  if (evbase->n_epoll > 0) {
    res = evbase->libc_epoll_wait(evbase->epfd, events, MAX_EVENTS, 0);

    for (i = 0; i < res; i++) {
      if ((events[i].events & EPOLLHUP) &&
          !(events[i].events & EPOLLRDHUP)) {
        ev = EV_READ | EV_WRITE;
      } else {
        if (events[i].events & EPOLLIN)
          ev |= EV_READ;
        if (events[i].events & EPOLLOUT)
          ev |= EV_WRITE;
        if (events[i].events & EPOLLRDHUP)
          ev |= EV_CLOSED;
      }

      evmap_io_active_(base, events[i].data.fd, ev);
    }
  }

  return 0;
}

static void
demievent_dealloc(struct event_base *base)
{
  struct demieventop *demiop = base->evbase;

  if (demiop != NULL) {
    memset(demiop, 0, sizeof(struct demieventop));
    mm_free(demiop);
  }

  return;
}

#endif /* EVENT__HAVE_DEMIEVENT */