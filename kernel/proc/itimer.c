#include <vex/atomic.h>
#include <vex/clocksrc.h>
#include <vex/itimer.h>
#include <vex/process.h>
#include <vex/signal.h>
#include <vex/spinlock.h>
#include <vex/time.h>

static spinlock_t itimer_lock = SPINLOCK_INIT;
static process_t* itimer_head; // Armed timers by deadline, under itimer_lock

static void insert_locked(process_t* restrict process)
{
    process_t** link = &itimer_head;

    while(*link && (*link)->itimer.deadline <= process->itimer.deadline) {
        link = &(*link)->itimer.next;
    }

    process->itimer.next = *link;
    *link = process;
}

static void remove_locked(process_t* restrict process)
{
    itimer_t* timer = &process->itimer;

    if(timer->deadline == 0) {
        return;
    }

    for(process_t** link = &itimer_head; *link; link = &(*link)->itimer.next) {
        if(*link == process) {
            *link = timer->next;
            break;
        }
    }

    timer->next = nullptr;
    timer->deadline = 0;
}

// The deadline that follows the one that passed, on a grid
// of whole intervals; the periods missed meanwhile are skipped
static uint64_t next_deadline(uint64_t deadline, uint64_t interval, uint64_t now)
{
    uint64_t periods = (now - deadline) / interval + 1;

    if(periods > (UINT64_MAX - deadline) / interval) {
        return UINT64_MAX;
    }

    return deadline + periods * interval;
}

void itimer_set(process_t* restrict process, const itimer_state_t* restrict next, itimer_state_t* restrict previous)
{
    itimer_t* timer = &process->itimer;
    uint64_t now = clock_source_now_ns();

    bool intr_state = spinlock_acquire_intr(&itimer_lock);

    if(previous) {
        previous->value = 0;
        previous->interval = timer->interval;

        // Armed but not yet signalled by the tick still has a moment to go
        if(timer->deadline > now) {
            previous->value = timer->deadline - now;
        }
        else if(timer->deadline) {
            previous->value = 1;
        }
    }

    if(next) {
        remove_locked(process);
        timer->interval = 0;

        // Checked under the lock itimer_cancel takes, so no timer outlives the process
        bool finished = atomic_load_n(&process->flags, ATOMIC_ACQUIRE) & PROCESS_EXIT_FINISHED;

        if(next->value && !finished) {
            timer->deadline = deadline_after_nsec(next->value);
            timer->interval = next->interval;
            insert_locked(process);
        }
    }

    spinlock_release_intr(&itimer_lock, intr_state);
}

void itimer_cancel(process_t* restrict process)
{
    bool intr_state = spinlock_acquire_intr(&itimer_lock);

    remove_locked(process);
    process->itimer.interval = 0;

    spinlock_release_intr(&itimer_lock, intr_state);
}

void itimer_run_expired(void)
{
    if(!atomic_load(&itimer_head, ATOMIC_RELAXED)) {
        return;
    }

    uint64_t now = clock_source_now_ns();

    raw_spinlock_acquire(&itimer_lock);

    while(itimer_head && itimer_head->itimer.deadline <= now) {
        process_t* process = itimer_head;
        itimer_t* timer = &process->itimer;

        itimer_head = timer->next;
        timer->next = nullptr;

        if(timer->interval) {
            timer->deadline = next_deadline(timer->deadline, timer->interval, now);
            insert_locked(process);
        }
        else {
            timer->deadline = 0;
        }

        // Allocates nothing, as the tick cannot;
        // the process cannot go while it is in the list
        signal_raise(process, SIGALRM);
    }

    raw_spinlock_release(&itimer_lock);
}
