#include <vex/atomic.h>
#include <vex/clocksrc.h>
#include <vex/errno.h>
#include <vex/hart.h>
#include <vex/itimer.h>
#include <vex/process.h>
#include <vex/signal.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/usage.h>

static spinlock_t itimer_lock = SPINLOCK_INIT;
static process_t* itimer_head; // Armed timers by deadline, under itimer_lock

static_assert(sizeof(itimer_cpu_set_t) <= SLAB_MAX_SIZE);

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

// Written under itimer_lock, and read without it to see whether a timer is armed
static void set_remaining(itimer_cpu_t* restrict timer, uint64_t remaining)
{
    atomic_store_n(&timer->remaining, remaining, ATOMIC_RELAXED);
}

// The periods that passed whole meanwhile are skipped, as for the real timer
static void count_down_locked(process_t* restrict process, itimer_cpu_t* restrict timer, uint64_t elapsed, int sig)
{
    if(timer->remaining == 0) {
        return;
    }

    if(elapsed < timer->remaining) {
        set_remaining(timer, timer->remaining - elapsed);
        return;
    }

    uint64_t excess = elapsed - timer->remaining;
    uint64_t remaining = 0;

    if(timer->interval) {
        remaining = timer->interval - excess % timer->interval;
    }

    set_remaining(timer, remaining);

    // Allocates nothing, as in the tick
    signal_raise(process, sig);
}

static bool cpu_armed(itimer_cpu_set_t* restrict cpu)
{
    uint64_t virtual_remaining = atomic_load_n(&cpu->timers[0].remaining, ATOMIC_RELAXED);
    uint64_t prof_remaining = atomic_load_n(&cpu->timers[1].remaining, ATOMIC_RELAXED);

    return virtual_remaining || prof_remaining;
}

// The run so far is system time, and the hart has
// charged it to the thread by the time this returns
static void settle_cpu(void)
{
    bool intr_state = disable_interrupts();

    hart_local_t* hart = this_hart();
    thread_t* thread = hart->curr_thread;

    usage_exit_kernel();
    itimer_count_cpu(hart, thread);

    restore_interrupts(intr_state);
}

static int set_cpu(process_t* restrict process, int which, const itimer_state_t* restrict next, itimer_state_t* restrict previous)
{
    settle_cpu();

    // Made before the lock, which nothing sleeps
    // under, and only for a timer that is armed
    itimer_cpu_set_t* fresh = nullptr;

    if(next && next->value && atomic_load_n(&process->itimer.cpu, ATOMIC_ACQUIRE) == nullptr) {
        fresh = slab_alloc(sizeof(itimer_cpu_set_t));

        if(fresh == nullptr) {
            return -ENOMEM;
        }

        memset(fresh, 0, sizeof(itimer_cpu_set_t));
    }

    bool intr_state = spinlock_acquire_intr(&itimer_lock);

    // Checked under the lock itimer_cancel takes, as for the real timer
    bool finished = atomic_load_n(&process->flags, ATOMIC_ACQUIRE) & PROCESS_EXIT_FINISHED;

    if(fresh && process->itimer.cpu == nullptr && !finished) {
        atomic_store_n(&process->itimer.cpu, fresh, ATOMIC_RELEASE);
        fresh = nullptr;
    }

    itimer_cpu_set_t* cpu = process->itimer.cpu;

    if(previous) {
        previous->value = 0;
        previous->interval = 0;

        if(cpu) {
            previous->value = cpu->timers[which - ITIMER_VIRTUAL].remaining;
            previous->interval = cpu->timers[which - ITIMER_VIRTUAL].interval;
        }
    }

    if(next && cpu) {
        itimer_cpu_t* timer = &cpu->timers[which - ITIMER_VIRTUAL];

        set_remaining(timer, 0);
        timer->interval = 0;

        if(next->value && !finished) {
            timer->interval = next->interval;
            set_remaining(timer, next->value);
        }
    }

    spinlock_release_intr(&itimer_lock, intr_state);

    slab_free(fresh);

    return 0;
}

static void set_real(process_t* restrict process, const itimer_state_t* restrict next, itimer_state_t* restrict previous)
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

int itimer_set(process_t* restrict process, int which, const itimer_state_t* restrict next, itimer_state_t* restrict previous)
{
    if(which == ITIMER_REAL) {
        set_real(process, next, previous);
        return 0;
    }

    return set_cpu(process, which, next, previous);
}

void itimer_cancel(process_t* restrict process)
{
    bool intr_state = spinlock_acquire_intr(&itimer_lock);

    remove_locked(process);
    process->itimer.interval = 0;

    itimer_cpu_set_t* cpu = process->itimer.cpu;

    for(unsigned i = 0; cpu && i < ITIMER_CPU_COUNT; ++i) {
        set_remaining(&cpu->timers[i], 0);
        cpu->timers[i].interval = 0;
    }

    spinlock_release_intr(&itimer_lock, intr_state);
}

void itimer_destroy(process_t* restrict process)
{
    slab_free(process->itimer.cpu);
}

void itimer_count_cpu(hart_local_t* restrict hart, thread_t* restrict thread)
{
    uint64_t user = hart->itimer_user_ns;
    uint64_t system = hart->itimer_system_ns;

    hart->itimer_user_ns = 0;
    hart->itimer_system_ns = 0;

    process_t* process = thread->process;

    if(process == nullptr) {
        return;
    }

    itimer_cpu_set_t* cpu = atomic_load_n(&process->itimer.cpu, ATOMIC_ACQUIRE);

    if(cpu == nullptr || !cpu_armed(cpu)) {
        return;
    }

    raw_spinlock_acquire(&itimer_lock);

    count_down_locked(process, &cpu->timers[0], user, SIGVTALRM);
    count_down_locked(process, &cpu->timers[1], user + system, SIGPROF);

    raw_spinlock_release(&itimer_lock);
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
