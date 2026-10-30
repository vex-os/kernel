#include <vex/atomic.h>
#include <vex/cputime.h>
#include <vex/cycles.h>
#include <vex/hart.h>
#include <vex/process.h>
#include <vex/resource.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/time.h>

// NOTENOTE: a thread's times change only on the hart that runs it,
// but another hart reads them, so each field is one atomic store

static uint64_t take_since_mark(hart_local_t* restrict hart)
{
    uint64_t now = local_clock_ns();
    uint64_t mark = hart->cputime_mark;
    hart->cputime_mark = now;

    // The mark can come from the clock source, before
    // init_cycles moves local_clock_ns to the cycle counter
    if(now < mark) {
        return 0;
    }

    return now - mark;
}

static void charge(uint64_t* restrict counter, uint64_t elapsed)
{
    uint64_t value = atomic_load_n(counter, ATOMIC_RELAXED);
    atomic_store_n(counter, value + elapsed, ATOMIC_RELAXED);
}

static void load_cputime(const thread_t* restrict thread, cputime_t* restrict out)
{
    out->user_ns = atomic_load_n(&thread->cputime.user_ns, ATOMIC_RELAXED);
    out->system_ns = atomic_load_n(&thread->cputime.system_ns, ATOMIC_RELAXED);
}

void cputime_enter_kernel(void)
{
    hart_local_t* hart = this_hart();
    uint64_t elapsed = take_since_mark(hart);
    charge(&hart->curr_thread->cputime.user_ns, elapsed);
}

void cputime_exit_kernel(void)
{
    hart_local_t* hart = this_hart();
    uint64_t elapsed = take_since_mark(hart);
    charge(&hart->curr_thread->cputime.system_ns, elapsed);
}

// The time of the idle thread goes nowhere for now
void cputime_switch(hart_local_t* restrict hart, thread_t* restrict curr)
{
    uint64_t elapsed = take_since_mark(hart);

    if(curr != hart->idle_thread) {
        charge(&curr->cputime.system_ns, elapsed);
    }
}

void cputime_of_thread(const thread_t* restrict thread, cputime_t* restrict out)
{
    // With interrupts on, a switch could charge the current run between the load and the mark
    bool intr_state = disable_interrupts();

    load_cputime(thread, out);

    if(thread == this_thread()) {
        // The current run is system time, as the caller is in the kernel
        hart_local_t* hart = this_hart();
        uint64_t now = local_clock_ns();

        if(now > hart->cputime_mark) {
            out->system_ns += now - hart->cputime_mark;
        }
    }

    restore_interrupts(intr_state);
}

void cputime_of_process(process_t* restrict process, cputime_t* restrict out)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    *out = process->cputime;

    for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
        cputime_t thread_time = {};
        cputime_of_thread(thread, &thread_time);
        cputime_add(out, &thread_time);
    }

    spinlock_release_intr(&process->lock, intr_state);
}

void cputime_to_rusage(const cputime_t* restrict time, rusage_t* restrict out)
{
    memset(out, 0, sizeof(*out));
    timeval_from_nsec(&out->ru_utime, time->user_ns);
    timeval_from_nsec(&out->ru_stime, time->system_ns);
}
