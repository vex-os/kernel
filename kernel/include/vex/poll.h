#pragma once

#include <usr/vex/poll.h>
#include <vex/assert.h>
#include <vex/compiler.h>
#include <vex/event.h>
#include <vex/types.h>

#define POLLIN     __k_POLLIN
#define POLLPRI    __k_POLLPRI
#define POLLOUT    __k_POLLOUT
#define POLLERR    __k_POLLERR
#define POLLHUP    __k_POLLHUP
#define POLLNVAL   __k_POLLNVAL
#define POLLRDNORM __k_POLLRDNORM
#define POLLRDBAND __k_POLLRDBAND
#define POLLWRNORM __k_POLLWRNORM
#define POLLWRBAND __k_POLLWRBAND
#define POLLMSG    __k_POLLMSG
#define POLLRDHUP  __k_POLLRDHUP

#define POLL_READABLE    (POLLIN | POLLRDNORM)
#define POLL_WRITABLE    (POLLOUT | POLLWRNORM)
#define POLL_TABLE_SLOTS 2 // The most events that the state of one file depends on

typedef struct __k_pollfd pollfd_t;
typedef struct poll_table_s poll_table_t;

struct poll_table_s {
    unsigned count;
    event_t* events[POLL_TABLE_SLOTS];
    uint32_t generations[POLL_TABLE_SLOTS];
};

/// Adds an event whose trigger can change what the poll of a file reports
/// @pre Called before the file reads the state that the event signals
static __always_inline void poll_table_add(poll_table_t* restrict table, event_t* restrict event)
{
    kassert(table->count < POLL_TABLE_SLOTS);

    table->events[table->count] = event;
    table->generations[table->count] = event_generation(event);
    table->count += 1;
}
