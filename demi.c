#include <stdlib.h>

#include <event2/util.h>
#include <event2/event.h>

#include "event-internal.h"
#include "changelist-internal.h"

#define EVENT__HAVE_DEMIEVENT 1
#ifdef EVENT__HAVE_DEMIEVENT

static void *
demievent_init(struct event_base *base);
static int
demievent_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);
static int
demievent_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo);
static int 
demievent_dispatch(struct event_base *base, struct timeval *);
static void
demievent_dealloc(struct event_base *base);

static const struct eventop demieventops_changelist = {
  "demievent",
  demievent_init,
  demievent_add,
  demievent_del,
  demievent_dispatch,
  demievent_dealloc,
  1, /* need reinit. not sure if this is necessary */
  EV_FEATURE_ET|EV_FEATURE_O1,
  EVENT_CHANGELIST_FDINFO_SIZE
};

const struct eventop demieventops = {
  "demievent",
  demievent_init,
  demievent_add,
  demievent_del,
  demievent_dispatch,
  demievent_dealloc,
  1,
  EV_FEATURE_ET|EV_FEATURE_O1,
  0
};

static void *
demievent_init(struct event_base *base)
{
  fprintf(stderr, "demievent_init/n");
  base->evsel = &demieventops_changelist;
  return NULL;
}

static int
demievent_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  fprintf(stderr, "demievent_add/n");
  return 0;
}

static int
demievent_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *fdinfo)
{
  fprintf(stderr, "demievent_del/n");
  return 0;
}

static int
demievent_dispatch(struct event_base *base, struct timeval *tv)
{
  fprintf(stderr, "demievent_dispatch/n");
  return 0;
}

static void
demievent_dealloc(struct event_base *base)
{
  fprintf(stderr, "demievent_dealloc/n");
  return;
}

#endif /* EVENT__HAVE_DEMIEVENT */