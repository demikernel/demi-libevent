// NOTE: libc requires this for RTLD_NEXT.
// #define _GNU_SOURCE 1

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/epoll.h>
#include <demi/libos.h>
#include <demi/wait.h>
#include <demi/types.h>

#include <event2/util.h>
#include <event2/event.h>

#include "event-internal.h"
#include "evthread-internal.h"
#include "evmap-internal.h"
#include "changelist-internal.h"

#ifdef EVENT__HAVE_DEMIEVENT
#define _POSIX_C_SOURCE 200809L

#define EVENT_BASE_FLAG_DEMIEVENT_USE_CHANGELIST 1
#define QTOKEN_MAX 65535

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
  /* Number of epoll events added */
  int n_epoll;
  /* Epoll holding non demikernel events */
  int epfd;
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
  // char *const args[] = {(char *const) "", (char *const) "catnap"};
  fprintf(stderr, "demievent_init\n");

  demiop = mm_calloc(1, sizeof(struct demieventop));
  if (!demiop) {
    fprintf(stderr, "demievent_init: return error\n");
    return NULL;
  }

  epfd = epoll_create1(0);
  if (epfd == -1) {
    fprintf(stderr, "demievent_init: epoll_create failed\n");
    return NULL;
  }
  demiop->epfd = epfd;

  // DEMIDO: Figure out if enabling changelist is better
  if (base->flags & EVENT_BASE_FLAG_DEMIEVENT_USE_CHANGELIST) {
    base->evsel = &demieventops_changelist;
  }

  base->evsel = &demieventops;
  
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
  int ret;
  struct epoll_event event;
  struct demieventop *evbase;

  evbase = (struct demieventop *) base->evbase;

  event.events = EPOLLIN;
  event.data.fd = fd;

  ret = epoll_ctl(evbase->epfd, EPOLL_CTL_ADD, fd, &event);

  if (ret == -1 && errno != EEXIST) {
    EINVAL;
    fprintf(stderr, "nochangelist_add_epoll: failed to add event errno=%d\n", errno);
    return -1;
  }

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
  // int ret;
  // struct demieventop *evbase;
  fprintf(stderr, "nochangelist_del_epoll\n");

  // evbase = (struct demieventop *) base->evbase;

  // ret = epoll_ctl(evbase->epfd, EPOLL_CTL_DEL, fd, NULL);

  // if (ret == -1 && errno == EBADF) {
  //   return -1;
  // } else if (ret == -1) {
  //   fprintf(stderr, "nochangelist_del_epoll: failed to delete event\n");
  //   return -1;
  // }

  return 0;
}

static int
demievent_dispatch(struct event_base *base, struct timeval *tv)
{
  int res, ev, i;
  struct demieventop *evbase;
  struct epoll_event events[20];
  long timeout = -1;

  evbase = (struct demieventop *) base->evbase;
  ev = 0;

  if (tv != NULL) {
    timeout = evutil_tv_to_msec_(tv);
  }

  res = epoll_wait(evbase->epfd, events, 20, timeout);

  if (res == -1)
    return -1;

  for (i = 0; i < res; i++)
  {
    if ((events[i].events & EPOLLHUP) && !(events[i].events & EPOLLRDHUP)) {
      ev = EV_READ | EV_WRITE;
    } else {
      if (events[i].events & EPOLLIN)
        ev |= EV_READ;
      if (events[i].events & EPOLLOUT)
        ev |= EV_WRITE;
      if (events[i].events & EPOLLRDHUP)
        ev |= EV_CLOSED;
    }

    evmap_io_active_(base, events[0].data.fd, ev);
  }


  return 0;
}

static void
demievent_dealloc(struct event_base *base)
{
  struct demieventop *demiop = base->evbase;
  fprintf(stderr, "demievent_dealloc\n");

  if (demiop != NULL) {
    memset(demiop, 0, sizeof(struct demieventop));
    mm_free(demiop);
  }

  return;
}

#endif /* EVENT__HAVE_DEMIEVENT */