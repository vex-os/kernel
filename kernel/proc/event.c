#include <vex/assert.h>
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

static int listen_locked(event_t* restrict event, thread_t* restrict thread, size_t wakeup_index)
{
    if(event->num_listeners >= event->max_listeners) {
        size_t new_max_listeners = 4;

        if(event->max_listeners) {
            new_max_listeners = event->max_listeners * 2;
        }

        event_listener_t* grown = krealloc(event->listeners, new_max_listeners * sizeof(event_listener_t));

        if(grown == nullptr) {
            return -ENOMEM;
        }

        event->listeners = grown;
        event->max_listeners = new_max_listeners;
    }

    event_listener_t* dest = &event->listeners[event->num_listeners];
    dest->wakeup_index = wakeup_index;
    dest->thread = thread;

    event->num_listeners += 1;

    return 0;
}

static bool unlisten_locked(event_t* restrict event, thread_t* restrict thread)
{
    for(size_t i = 0; i < event->num_listeners; ++i) {
        if(event->listeners[i].thread == thread) {
            event->num_listeners -= 1;
            event->listeners[i] = event->listeners[event->num_listeners];
            return true;
        }
    }

    return false;
}

// A trigger takes every listener off its event,
// so a thread still listening everywhere was not triggered
static bool unlisten_all(thread_t* restrict thread)
{
    bool triggered = false;

    for(size_t i = 0; i < thread->num_events; ++i) {
        event_t* event = thread->events[i];

        spinlock_acquire(&event->lock);
        bool listening = unlisten_locked(event, thread);
        spinlock_release(&event->lock);

        if(!listening) {
            triggered = true;
        }
    }

    thread->num_events = 0;

    return triggered;
}

static int reserve_events(thread_t* restrict thread, size_t count)
{
    if(count <= thread->max_events) {
        return 0;
    }

    event_t** grown = krealloc(thread->events, count * sizeof(event_t*));

    if(grown == nullptr) {
        return -ENOMEM;
    }

    thread->events = grown;
    thread->max_events = count;

    return 0;
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

    if(reserve_events(thread, count) < 0) {
        release_locks(events, count, lock);
        restore_interrupts(intr_state);
        return -ENOMEM;
    }

    for(size_t i = 0; i < count; ++i) {
        if(listen_locked(events[i], thread, i) < 0) {
            for(size_t j = 0; j < i; ++j) {
                unlisten_locked(events[j], thread);
            }

            release_locks(events, count, lock);
            restore_interrupts(intr_state);
            return -ENOMEM;
        }

        thread->events[i] = events[i];
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

    if(!unlisten_all(thread)) {
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

    atomic_store(&event->generation, event->generation + 1, ATOMIC_RELEASE);

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
    event->generation = 0;
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

uint32_t event_generation(event_t* restrict event)
{
    return atomic_load(&event->generation, ATOMIC_ACQUIRE);
}

int event_watch_begin(event_watch_t* restrict watch, size_t count)
{
    thread_t* thread = this_thread();

    watch->intr_state = disable_interrupts();

    if(reserve_events(thread, count) < 0) {
        restore_interrupts(watch->intr_state);
        return -ENOMEM;
    }

    watch->moved = false;
    watch->error = 0;
    thread->num_events = 0;

    // From here on a trigger of an event that the thread listens on makes it runnable
    // again, so that one which comes between two adds or before the yield is not lost
    sched_dequeue(thread, true);

    return 0;
}

void event_watch_add(event_watch_t* restrict watch, event_t* restrict event, uint32_t generation)
{
    thread_t* thread = this_thread();

    if(watch->moved || watch->error) {
        return;
    }

    kassert(thread->num_events < thread->max_events);

    spinlock_acquire(&event->lock);

    if(event->generation != generation) {
        watch->moved = true;
        spinlock_release(&event->lock);
        return;
    }

    watch->error = listen_locked(event, thread, thread->num_events);

    if(watch->error == 0) {
        thread->events[thread->num_events] = event;
        thread->num_events += 1;
    }

    spinlock_release(&event->lock);
}

ssize_t event_watch_wait(event_watch_t* restrict watch, uint64_t deadline)
{
    thread_t* thread = this_thread();
    bool waits = !watch->moved && watch->error == 0;

    // A thread that runs is put back the same way a trigger puts it back
    if(!waits) {
        sched_enqueue(thread);
    }
    else if(deadline) {
        sched_wake_at(thread, deadline);
    }

    restore_interrupts(watch->intr_state);

    sched_yield();

    if(waits && deadline) {
        sched_wake_cancel(thread);
    }

    bool triggered = unlisten_all(thread);

    if(watch->error) {
        return watch->error;
    }

    if(triggered || watch->moved) {
        return 0;
    }

    if(atomic_load(&thread->pending_work, ATOMIC_ACQUIRE)) {
        return -ERESTART;
    }

    if(deadline) {
        return -ETIMEDOUT;
    }

    return 0;
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
