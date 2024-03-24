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
#include "evmap-internal.h"
#include "changelist-internal.h"

#ifdef EVENT__HAVE_DEMIEVENT
#define _POSIX_C_SOURCE 200809L

#define EVENT_BASE_FLAG_DEMIEVENT_USE_CHANGELIST 1
#define QTOKEN_MAX 65535

static void *
demievent_init(struct event_base *base);

static int
demievent_changelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int
demievent_changelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int
demievent_nochangelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int
nochangelist_add_demikernel(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int
nochangelist_add_epoll(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int
demievent_nochangelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int
nochangelist_del_demikernel(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int nochangelist_del_epoll(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int 
demievent_dispatch(struct event_base *base, struct timeval *);

static int 
dispatch_demikernel(struct event_base *base, struct timeval *tv);

static int 
dispatch_epoll(struct event_base *base, struct timeval *tv);

static void
demievent_dealloc(struct event_base *base);

struct demivent {
  int fd;
  short events;
  demi_qtoken_t r_qt;
  demi_qtoken_t w_qt;
  int next;
  int prev;
};

struct demieventop {
  /* Set to 1 if we should check demikernel first
     for dispatch. Toggled between dispatch calls
     so that both demikernel and epoll get a fair
     usage */
  int demikernel_first;

  /* Fields for epoll events */
  /* Number of epoll events added */
  int n_epoll;
  /* Epoll holding non demikernel events */
  int epfd;

  /* Fields for demikernel events */
  /* Number of demivents added */
  int n_demi;
  /* Head of demivent list. If head is -1 list is empty */
  int head;
  /* Preallocated array of demivents */
  struct demivent demivents[QTOKEN_MAX];
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
  int i, epfd;
  struct demieventop *demiop;
  char *const args[] = {(char *const) "", (char *const) "catnap"};
  fprintf(stderr, "demievent_init\n");

  // Demikernel related initialization
  if (demi_init(2, args) != 0) {
    fprintf(stderr, "demievent_init: failed to initialize demikernel\n");
    abort();
  }

  demiop = mm_calloc(1, sizeof(struct demieventop));
  if (!demiop) {
    fprintf(stderr, "demievent_init: return error\n");
    return NULL;
  }

  demiop->n_demi = 0;
  demiop->head = -1;
  demiop->demikernel_first = 1;

  // Initialize all demivents to an fd of -1
  for (i = 0; i < QTOKEN_MAX; i++) {
    demiop->demivents->fd = -1;
    demiop->demivents->events = 0;
  }

  // Epoll related initialization
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
  
  fprintf(stderr, "demievent_init return\n");
  return (demiop);
}

static int
demievent_changelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  fprintf(stderr, "demievent_changelist_add\n");
  abort();
  return 0;
}

static int
demievent_nochangelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  int ret;
  fprintf(stderr, "demievent_nochangelist_add\n");

  // If fd >= 500 it is a demikernel descriptor
  if (fd >= 500) {
    ret = nochangelist_add_demikernel(base, fd, old, events, fdinfo);
  } else {
    ret = nochangelist_add_epoll(base, fd, old, events, fdinfo);
  }

  return ret;
}

static int
nochangelist_add_demikernel(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  demi_qtoken_t r_qt;
  struct demieventop *evbase;
  struct demivent new_ev;

  evbase = (struct demieventop *) base->evbase;
  
  new_ev = evbase->demivents[fd];
  new_ev.events = events;

  // This event was not a read event so need a read token
  if ((events & EV_READ) && ~(new_ev.events & EV_READ)) {
    if (demi_pop(&r_qt, fd) != 0) {
      fprintf(stderr, "demievent_nochangelist_add: demipop failed\n");
      return -1;
    }

    new_ev.r_qt = r_qt;
  }

  // This event was not a write event so need a write token
  // if ((events & EV_WRITE) && ~(new_ev.events) & EV_WRITE) {
  //   if (demi_push(&w_qt, fd, res) != 0)
  //     return -1;

  //   new_ev.w_qt = w_qt;
  // }

  /* If this event already existed and we were just
   * updating it return, so we don't add it to list again.
  */
  if (new_ev.fd != -1)
    return 0;

  // New event is head
  if (evbase->head == -1) {
    evbase->head = fd;
  } else {
    // New event is not head
    evbase->demivents[evbase->head].prev = fd;
    new_ev.next = evbase->head;
    evbase->head = fd;
  }

  evbase->n_demi++;
  new_ev.fd = fd;

  return 0;
}

static int
nochangelist_add_epoll(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  int ret;
  struct epoll_event event;
  struct demieventop *evbase;

  evbase = (struct demieventop *) base->evbase;

  event.events = EPOLLIN;
  event.data.fd = fd;

  ret = epoll_ctl(evbase->epfd, EPOLL_CTL_ADD, fd, &event);
  if (ret == -1) {
    fprintf(stderr, "nochangelist_add_epoll: failed to add event\n");
    fprintf(stderr, "fd=%d epfd=%d\n", fd, evbase->epfd);
    return -1;
  }

  return 0;
}

static int
demievent_changelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  fprintf(stderr, "demievent_changelist_del\n");
  abort();
  return 0;
}

static int
demievent_nochangelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  int ret;
  fprintf(stderr, "demievent_nochangelist_del\n");

    // If fd >= 500 it is a demikernel descriptor
  if (fd >= 500) {
    ret = nochangelist_del_demikernel(base, fd, old, events, fdinfo);
  } else {
    ret = nochangelist_del_epoll(base, fd, old, events, fdinfo);
  }

  return ret;
}

static int
nochangelist_del_demikernel(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  struct demieventop *evbase;
  int prev, next;

  evbase = (struct demieventop *) base->evbase;

  // Event wasn't added so can't delete it
  if (evbase->demivents[fd].fd == -1)
    return -1;

  prev = evbase->demivents[fd].prev;
  next = evbase->demivents[fd].next;

  // If event is not head of list
  if (prev != -1)
    evbase->demivents[prev].next = next;

  // If event is not tail of list
  if (next != -1)
    evbase->demivents[next].prev = prev;

  // Deleted event was head of list so make next new head
  if (evbase->head == fd)
    evbase->head = next;

  evbase->demivents[fd].fd = -1;
  evbase->demivents[fd].next = -1;
  evbase->demivents[fd].prev = -1;
  evbase->demivents[fd].events = 0;
  evbase->n_demi--;

  return 0;
}

static int nochangelist_del_epoll(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  int ret;
  struct demieventop *evbase;

  evbase = (struct demieventop *) base->evbase;

  ret = epoll_ctl(evbase->epfd, EPOLL_CTL_DEL, fd, NULL);
  if (ret == -1) {
    fprintf(stderr, "nochangelist_del_epoll: failed to delete event\n");
    return -1;
  }

  return 0;
}

static int
demievent_dispatch(struct event_base *base, struct timeval *tv)
{
  int ret;
  struct demieventop *evbase;

  evbase = (struct demieventop *) base->evbase;

  if (evbase->demikernel_first) {
    ret = dispatch_demikernel(base, tv);
    evbase->demikernel_first = 0;
  } else {
    ret = dispatch_epoll(base, tv);
    evbase->demikernel_first = 1;
  }

  return ret;
}

static int 
dispatch_demikernel(struct event_base *base, struct timeval *tv)
{
  int i, cur, total;
  short events;
  struct demivent ev;
  demi_qresult_t qr = {0};
  struct demieventop *evbase;
  struct timespec timeout;
  struct timespec *timeout_ptr;
  fprintf(stderr, "demievent_dispatch\n");
  
  evbase = (struct demieventop *) base->evbase;
  events = 0;

  // Set timeout
  if (tv) {
    timeout.tv_sec = tv->tv_sec;
    timeout.tv_nsec = tv->tv_usec * 1000;
    timeout_ptr = &timeout;
  } else {
    timeout_ptr = NULL;
  }

  // DEMIDO: Don't always start at the same event
  cur = evbase->head;
  total = 0;
  for (i = 0; i < evbase->n_demi; i++) {
    ev = evbase->demivents[cur];

    /* DEMIDO: Check whether tv==NULL means ininite timeout
    * or return immediately.
    */
    if (evbase->demivents[cur].events & EV_READ) {
      if (demi_wait(&qr, ev.r_qt, timeout_ptr) != 0)
        return -1;
      else
        events |= EV_READ;
    }

    // if (evbase->demivents[cur].events & EV_WRITE) {
    //   if (demi_wait(&res, ev.w_qt, timeout_ptr) != 0)
    //     return -1;
    //   else
    //     events |= EV_WRITE;
    // }

    if (events != 0) {
      evmap_io_active_(base, ev.fd, events);
      total += 1;
    }

    cur = ev.next;
  }

  if (total == 0)
    return -1;
  else
    return 0;
}

static int 
dispatch_epoll(struct event_base *base, struct timeval *tv)
{
  int res, ev;
  struct demieventop *evbase;
  struct epoll_event events[1];
  long timeout = -1;

  evbase = (struct demieventop *) base->evbase;
  ev = 0;

  if (tv != NULL) {
    timeout = evutil_tv_to_msec_(tv);
  }

  res = epoll_wait(evbase->epfd, events, 1, timeout);

  if (res & EPOLLERR) {
    ev = EV_READ | EV_WRITE;
  } else if ((res & EPOLLHUP) && !(res & EPOLLRDHUP)) {
    ev = EV_READ | EV_WRITE;
  } else {
    if (res & EPOLLIN)
      ev |= EV_READ;
    if (res & EPOLLOUT)
      ev |= EV_WRITE;
    if (res & EPOLLRDHUP)
      ev |= EV_CLOSED;
  }

  if (res > 0) {
    evmap_io_active_(base, events[0].data.fd, ev);
  }

  if (res >= 0)
    return -1;
  else
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