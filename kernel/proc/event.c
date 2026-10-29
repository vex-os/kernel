#include <vex/atomic.h>
#include <vex/clocksrc.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/hart.h>
#include <vex/kmalloc.h>
#include <vex/panic.h>
#include <vex/sched.h>
#include <vex/slab.h>
#include <vex/thread.h>
#include <vex/time.h>

static_assert(sizeof(event_t) <= SLAB_MAX_SIZE);

static event_deferral_t* deferred_head = nullptr;

static void release_locks(event_t** restrict events, size_t count, spinlock_t* restrict lock)
{
    for(size_t i = 0; i < count; ++i) {
        spinlock_release(&events[i]->lock);
    }

    if(lock) {
        raw_spinlock_release(lock);
    }
}

// A deadline of zero waits forever. A lock, held by the caller as spinlock_acquire_intr
// left it, is released only once the thread listens, together with the events' locks
static ssize_t await_until(event_t** restrict events, size_t count, uint64_t deadline, bool interruptible, spinlock_t* restrict lock,
    bool intr_state)
{
    thread_t* thread = this_thread();

    if(lock == nullptr) {
        ssize_t result = event_await(events, count);

        if(result >= 0) {
            return result;
        }

        intr_state = disable_interrupts();
    }

    for(size_t i = 0; i < count; ++i) {
        spinlock_acquire(&events[i]->lock);
    }

    // Something may have become pending since event_await dropped the locks
    for(size_t i = 0; i < count; ++i) {
        if(events[i]->num_pending > 0) {
            events[i]->num_pending -= 1;
            release_locks(events, count, lock);
            restore_interrupts(intr_state);
            return i;
        }
    }

    if(count > thread->max_events) {
        event_t** grown = krealloc(thread->events, count * sizeof(event_t*));

        if(grown == nullptr) {
            release_locks(events, count, lock);
            restore_interrupts(intr_state);
            return -ENOMEM;
        }

        thread->events = grown;
        thread->max_events = count;
    }

    for(size_t i = 0; i < count; ++i) {
        event_t* event = events[i];

        if(event->num_listeners >= event->max_listeners) {
            size_t new_max_listeners = 4;

            if(event->max_listeners) {
                new_max_listeners = event->max_listeners * 2;
            }

            event_listener_t* grown = krealloc(event->listeners, new_max_listeners * sizeof(event_listener_t));

            if(grown == nullptr) {
                for(size_t j = 0; j < i; ++j) {
                    event_t* registered = events[j];

                    registered->num_listeners -= 1;

                    for(size_t k = 0; k < registered->num_listeners; ++k) {
                        if(registered->listeners[k].thread == thread) {
                            registered->listeners[k] = registered->listeners[registered->num_listeners];
                            break;
                        }
                    }
                }

                release_locks(events, count, lock);
                restore_interrupts(intr_state);
                return -ENOMEM;
            }

            event->listeners = grown;
            event->max_listeners = new_max_listeners;
        }

        event_listener_t* dest = &event->listeners[event->num_listeners++];
        dest->wakeup_index = i;
        dest->thread = thread;

        thread->events[i] = event;
    }

    thread->num_events = count;

    sched_dequeue(thread, interruptible);

    if(deadline) {
        sched_wake_at(thread, deadline);
    }

    release_locks(events, count, lock);

    // sched_yield waits for its own IPI, so interrupts have to be back on first
    restore_interrupts(intr_state);

    sched_yield();

    if(deadline) {
        sched_wake_cancel(thread);
    }

    // A trigger takes every listener off its event,
    // so a thread still listening everywhere timed out
    bool triggered = false;

    for(size_t i = 0; i < thread->num_events; ++i) {
        event_t* event = thread->events[i];
        bool listening = false;

        spinlock_acquire(&event->lock);

        for(size_t j = 0; j < event->num_listeners; ++j) {
            if(event->listeners[j].thread == thread) {
                event->num_listeners -= 1;
                event->listeners[j] = event->listeners[event->num_listeners];
                listening = true;
                break;
            }
        }

        spinlock_release(&event->lock);

        if(!listening) {
            triggered = true;
        }
    }

    thread->num_events = 0;

    if(!triggered) {
        if(interruptible && atomic_load(&thread->pending_work, ATOMIC_ACQUIRE)) {
            return -ERESTART;
        }

        return -ETIMEDOUT;
    }

    return thread->wakeup_index;
}

static size_t trigger(event_t* restrict event, uint32_t max_pending)
{
    bool intr_state = disable_interrupts();

    spinlock_acquire(&event->lock);

    if(event->num_listeners == 0) {
        if(event->num_pending < max_pending) {
            event->num_pending += 1;
        }

        spinlock_release(&event->lock);
        restore_interrupts(intr_state);
        return 0;
    }

    for(size_t i = 0; i < event->num_listeners; ++i) {
        event_listener_t* dest = &event->listeners[i];
        thread_t* thread = dest->thread;

        thread->wakeup_index = dest->wakeup_index;

        sched_enqueue(thread);
    }

    size_t num_handled = event->num_listeners;

    event->num_listeners = 0;

    spinlock_release(&event->lock);
    restore_interrupts(intr_state);

    return num_handled;
}

void event_init(event_t* restrict event)
{
    event->lock = SPINLOCK_INIT;
    event->num_pending = 0;
    event->num_listeners = 0;
    event->max_listeners = 0;
    event->listeners = nullptr;
}

void event_deinit(event_t* restrict event)
{
    kfree(event->listeners);

    event->num_pending = 0;
    event->num_listeners = 0;
    event->max_listeners = 0;
    event->listeners = nullptr;
}

ssize_t event_await(event_t** restrict events, size_t count)
{
    bool intr_state = disable_interrupts();

    for(size_t i = 0; i < count; ++i) {
        spinlock_acquire(&events[i]->lock);
    }

    ssize_t result = -EAGAIN;

    for(size_t i = 0; i < count; ++i) {
        if(events[i]->num_pending > 0) {
            events[i]->num_pending -= 1;
            result = i;
            break;
        }
    }

    for(size_t i = 0; i < count; ++i) {
        spinlock_release(&events[i]->lock);
    }

    restore_interrupts(intr_state);

    return result;
}

ssize_t event_await_blocking(event_t** restrict events, size_t count)
{
    return await_until(events, count, 0, false, nullptr, false);
}

ssize_t event_await_timeout(event_t** restrict events, size_t count, uint64_t timeout_nsec)
{
    uint64_t deadline = deadline_after_nsec(timeout_nsec);

    return await_until(events, count, deadline, false, nullptr, false);
}

ssize_t event_await_interruptible(event_t** restrict events, size_t count)
{
    return await_until(events, count, 0, true, nullptr, false);
}

ssize_t event_await_unlock(event_t** restrict events, size_t count, spinlock_t* restrict lock, bool intr_state, uint64_t timeout_nsec)
{
    uint64_t deadline = 0;

    if(timeout_nsec) {
        deadline = deadline_after_nsec(timeout_nsec);
    }

    return await_until(events, count, deadline, true, lock, intr_state);
}

ssize_t event_await_blocking_unlock(event_t** restrict events, size_t count, spinlock_t* restrict lock, bool intr_state)
{
    return await_until(events, count, 0, false, lock, intr_state);
}

ssize_t event_sleep(uint64_t timeout_nsec)
{
    event_t event;
    event_init(&event);

    event_t* event_pointer = &event;
    spinlock_t lock = SPINLOCK_INIT;
    bool intr_state = spinlock_acquire_intr(&lock);
    ssize_t result = event_await_unlock(&event_pointer, 1, &lock, intr_state, timeout_nsec);

    event_deinit(&event);

    if(result == -ETIMEDOUT) {
        return 0;
    }

    return result;
}

size_t event_trigger(event_t* restrict event, bool linger_pending)
{
    if(linger_pending) {
        return trigger(event, UINT32_MAX);
    }

    return trigger(event, 0U);
}

void event_reset(event_t* restrict event)
{
    bool intr_state = spinlock_acquire_intr(&event->lock);
    event->num_pending = 0;
    spinlock_release_intr(&event->lock, intr_state);
}

void event_trigger_deferred(event_deferral_t* restrict deferral)
{
    if(atomic_exchange(&deferral->queued, true, ATOMIC_ACQ_REL)) {
        return;
    }

    event_deferral_t* head = atomic_load(&deferred_head, ATOMIC_RELAXED);

    do {
        deferral->next = head;
    } while(!atomic_cmpxchg_weak(&deferred_head, &head, deferral, ATOMIC_RELEASE, ATOMIC_RELAXED));
}

void event_run_deferred(void)
{
    event_deferral_t* it = atomic_exchange(&deferred_head, nullptr, ATOMIC_ACQUIRE);

    while(it) {
        event_deferral_t* next = it->next;

        // Unqueued, the deferral may be queued again over its next, so that's read first;
        // a deferred trigger after the store queues it again, one before is served below
        atomic_store(&it->queued, false, ATOMIC_RELEASE);

        trigger(it->event, 1U);

        it = next;
    }
}
