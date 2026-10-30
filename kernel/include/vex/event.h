#pragma once

#include <vex/spinlock.h>
#include <vex/types.h>

typedef struct thread_s thread_t;

typedef struct event_listener_s event_listener_t;
typedef struct event_s event_t;
typedef struct event_deferral_s event_deferral_t;
typedef struct event_watch_s event_watch_t;

struct event_listener_s {
    thread_t* thread;
    size_t wakeup_index;
};

struct event_s {
    spinlock_t lock;
    uint32_t num_pending;
    uint32_t num_listeners;
    uint32_t max_listeners;
    uint32_t generation; // Counts the triggers and wraps around; read with event_generation
    event_listener_t* listeners;
};

struct event_deferral_s {
    event_deferral_t* next;
    event_t* event;
    bool queued;
};

struct event_watch_s {
    bool intr_state;
    bool moved; // An event was triggered between event_generation and event_watch_add
    int error;
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

/// Reads the count of triggers of the event, for event_watch_add
/// @note Read it before the check of the condition that the event signals
uint32_t event_generation(event_t* restrict event);

/// Starts a wait on several events whose conditions the caller checked under different locks.
/// The steps: event_generation for each event, the checks, event_watch_begin,
/// event_watch_add for each event, then event_watch_wait
/// @pre The caller holds no spinlock
/// @param count The most events that event_watch_add gets
/// @retval -ENOMEM No memory to register the thread as a listener
/// @note Takes no lock on all events at once, so two waits that list the same events in another order can't deadlock
int event_watch_begin(event_watch_t* restrict watch, size_t count);

/// @pre event_watch_begin succeeded, and this is not one add more than its count
/// @param generation What event_generation gave before the check of the condition
/// @note A trigger since that generation makes event_watch_wait return at once
void event_watch_add(event_watch_t* restrict watch, event_t* restrict event, uint32_t generation);

/// Sleeps until an event from event_watch_add is triggered, or was triggered since its generation
/// @pre event_watch_begin succeeded
/// @param deadline Zero waits for as long as it takes
/// @retval 0 An event was triggered; the caller checks its conditions again, as the trigger may be for another one
/// @retval -ETIMEDOUT The deadline passed
/// @retval -ENOMEM No memory to register the thread as a listener
/// @retval -ERESTART Work is pending; a system call returns this
/// as it is and runs again or fails with EINTR on the way out
ssize_t event_watch_wait(event_watch_t* restrict watch, uint64_t deadline);

/// Triggers the event from the next scheduler tick on any hart, keeping at most one wakeup for the next await
/// @note Takes no lock, so it works where event_trigger cannot: in code that can run with scheduler locks held
void event_trigger_deferred(event_deferral_t* restrict deferral);

/// Triggers the events of the deferrals queued so far
/// @pre Called by the scheduler tick, with interrupts disabled
void event_run_deferred(void);
