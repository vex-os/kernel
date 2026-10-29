#pragma once

#include <vex/spinlock.h>
#include <vex/types.h>

typedef struct thread_s thread_t;

typedef struct event_listener_s event_listener_t;
typedef struct event_s event_t;
typedef struct event_deferral_s event_deferral_t;

struct event_listener_s {
    thread_t* thread;
    size_t wakeup_index;
};

struct event_s {
    spinlock_t lock;
    uint32_t num_pending;
    uint32_t num_listeners;
    uint32_t max_listeners;
    event_listener_t* listeners;
};

struct event_deferral_s {
    event_deferral_t* next;
    event_t* event;
    bool queued;
};

void event_init(event_t* restrict event);
void event_deinit(event_t* restrict event);

/// Takes a pending wakeup from the first event that has one, without blocking
/// @return The index of that event in events
/// @retval -EAGAIN No event has a pending wakeup
ssize_t event_await(event_t** restrict events, size_t count);

/// Waits until one of the events is triggered, or takes a pending wakeup
/// @return The index of the event in events
/// @retval -ENOMEM No memory to register the thread as a listener
ssize_t event_await_blocking(event_t** restrict events, size_t count);

/// Waits until one of the events is triggered, or takes a pending wakeup, for at most the timeout
/// @return The index of the event in events
/// @retval -ETIMEDOUT No event was triggered in time
/// @retval -ENOMEM No memory to register the thread as a listener
ssize_t event_await_timeout(event_t** restrict events, size_t count, uint64_t timeout_nsec);

/// Waits until one of the events is triggered, or takes a pending wakeup,
/// ending early for pending work such as a signal
/// @return The index of the event in events
/// @retval -ENOMEM No memory to register the thread as a listener
/// @retval -ERESTART Work is pending; a system call returns this
/// as it is and runs again or fails with EINTR on the way out
ssize_t event_await_interruptible(event_t** restrict events, size_t count);

/// event_await_interruptible that releases the lock guarding the condition waited for only once
/// the thread listens, so a trigger made under that lock after the caller checked can't be missed
/// @pre The caller holds the lock, taken with spinlock_acquire_intr, which returned intr_state
/// @post The lock is released and interrupts are back to intr_state, whatever the result
/// @param timeout_nsec Zero waits for as long as it takes
/// @return The index of the event in events
/// @retval -ETIMEDOUT No event was triggered in time
/// @retval -ENOMEM No memory to register the thread as a listener
/// @retval -ERESTART Work is pending; a system call returns this
/// as it is and runs again or fails with EINTR on the way out
ssize_t event_await_unlock(event_t** restrict events, size_t count, spinlock_t* restrict lock, bool intr_state, uint64_t timeout_nsec);

/// event_await_unlock that only a trigger ends: pending work such as a signal does not
/// @pre The caller holds the lock, taken with spinlock_acquire_intr, which returned intr_state
/// @post The lock is released and interrupts are back to intr_state, whatever the result
/// @return The index of the event in events
/// @retval -ENOMEM No memory to register the thread as a listener
ssize_t event_await_blocking_unlock(event_t** restrict events, size_t count, spinlock_t* restrict lock, bool intr_state);

/// Sleeps until the timeout passes or pending work such as a signal arrives
/// @param timeout_nsec Zero sleeps for as long as it takes
/// @retval 0 The timeout passed
/// @retval -ERESTART Work is pending; a system call returns this
/// as it is and runs again or fails with EINTR on the way out
ssize_t event_sleep(uint64_t timeout_nsec);

/// Wakes every thread waiting on the event
/// @param linger_pending With nobody waiting, keep the wakeup for the next await
/// instead; each call adds one, like a semaphore
/// @return The number of threads woken
size_t event_trigger(event_t* restrict event, bool linger_pending);

void event_reset(event_t* restrict event);

/// Triggers the event from the next scheduler tick on any hart, keeping at most one wakeup for the next await
/// @note Takes no lock, so it works where event_trigger cannot: in code that can run with scheduler locks held
void event_trigger_deferred(event_deferral_t* restrict deferral);

/// Triggers the events of the deferrals queued so far
/// @pre Called by the scheduler tick, with interrupts disabled
void event_run_deferred(void);
