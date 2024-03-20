#include <stdlib.h>
#include <assert.h>
#include <string.h>
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
demievent_nochangelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);

static int 
demievent_dispatch(struct event_base *base, struct timeval *);

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
  /* Number of demivents added */
  int nevents;
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
  int i;
  struct demieventop *demiop;
  fprintf(stderr, "demievent_init\n");

  demiop = mm_calloc(1, sizeof(struct demieventop));
  if (!demiop) {
    return NULL;
  }

  demiop->nevents = 0;
  demiop->head = -1;

  // Initialize all demivents to an fd of -1
  for (i = 0; i < QTOKEN_MAX; i++) {
    demiop->demivents->fd = -1;
    demiop->demivents->events = 0;
  }

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
  fprintf(stderr, "demievent_changelist_add\n");
  abort();
  return 0;
}

static int
demievent_nochangelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  demi_qtoken_t r_qt;
  struct demieventop *evsel;
  struct demivent new_ev;
  fprintf(stderr, "demievent_nochangelist_add\n");

  evsel = (struct demieventop *) base->evsel;
  
  new_ev = evsel->demivents[fd];
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
  if (evsel->head == -1) {
    evsel->head = fd;
  } else {
    // New event is not head
    evsel->demivents[evsel->head].prev = fd;
    new_ev.next = evsel->head;
    evsel->head = fd;
  }

  evsel->nevents++;
  new_ev.fd = fd;

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
  struct demieventop *evsel;
  int prev, next;
  fprintf(stderr, "demievent_nochangelist_del\n");

  evsel = (struct demieventop *) base->evsel;

  // Event wasn't added so can't delete it
  if (evsel->demivents[fd].fd == -1)
    return -1;

  prev = evsel->demivents[fd].prev;
  next = evsel->demivents[fd].next;

  // If event is not head of list
  if (prev != -1)
    evsel->demivents[prev].next = next;

  // If event is not tail of list
  if (next != -1)
    evsel->demivents[next].prev = prev;

  // Deleted event was head of list so make next new head
  if (evsel->head == fd)
    evsel->head = next;

  evsel->demivents[fd].fd = -1;
  evsel->demivents[fd].next = -1;
  evsel->demivents[fd].prev = -1;
  evsel->demivents[fd].events = 0;
  evsel->nevents--;

  return 0;
}

static int
demievent_dispatch(struct event_base *base, struct timeval *tv)
{
  int i, cur, total;
  short events;
  struct demivent ev;
  demi_qresult_t qr = {0};
  struct demieventop *evsel;
  struct timespec timeout;
  struct timespec *timeout_ptr;
  fprintf(stderr, "demievent_dispatch\n");
  
  evsel = (struct demieventop *) base->evsel;
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
  cur = evsel->head;
  total = 0;
  for (i = 0; i < evsel->nevents; i++) {
    ev = evsel->demivents[cur];

    /* DEMIDO: Check whether tv==NULL means ininite timeout
    * or return immediately.
    */
    if (evsel->demivents[cur].events & EV_READ) {
      if (demi_wait(&qr, ev.r_qt, timeout_ptr) != 0)
        return -1;
      else
        events |= EV_READ;
    }

    // if (evsel->demivents[cur].events & EV_WRITE) {
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