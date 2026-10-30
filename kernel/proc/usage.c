#include <vex/atomic.h>
#include <vex/cycles.h>
#include <vex/hart.h>
#include <vex/page.h>
#include <vex/process.h>
#include <vex/resource.h>
#include <vex/rmap.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/usage.h>
#include <vex/vms.h>

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

static void load_usage(const thread_t* restrict thread, usage_t* restrict out)
{
    out->user_ns = atomic_load_n(&thread->usage.user_ns, ATOMIC_RELAXED);
    out->system_ns = atomic_load_n(&thread->usage.system_ns, ATOMIC_RELAXED);
    out->minor_faults = atomic_load_n(&thread->usage.minor_faults, ATOMIC_RELAXED);
    out->major_faults = atomic_load_n(&thread->usage.major_faults, ATOMIC_RELAXED);
    out->blocks_in = atomic_load_n(&thread->usage.blocks_in, ATOMIC_RELAXED);
    out->blocks_out = atomic_load_n(&thread->usage.blocks_out, ATOMIC_RELAXED);
    out->voluntary_switches = atomic_load_n(&thread->usage.voluntary_switches, ATOMIC_RELAXED);
    out->involuntary_switches = atomic_load_n(&thread->usage.involuntary_switches, ATOMIC_RELAXED);
    out->signals = atomic_load_n(&thread->usage.signals, ATOMIC_RELAXED);
}

void usage_enter_kernel(void)
{
    hart_local_t* hart = this_hart();
    uint64_t elapsed = take_since_mark(hart);
    usage_increase(&hart->curr_thread->usage.user_ns, elapsed);
    hart->itimer_user_ns += elapsed;
}

void usage_exit_kernel(void)
{
    hart_local_t* hart = this_hart();
    uint64_t elapsed = take_since_mark(hart);
    usage_increase(&hart->curr_thread->usage.system_ns, elapsed);
    hart->itimer_system_ns += elapsed;
}

// The time of the idle thread goes nowhere for now
void usage_switch(hart_local_t* restrict hart, thread_t* restrict curr)
{
    uint64_t elapsed = take_since_mark(hart);

    if(curr != hart->idle_thread) {
        usage_increase(&curr->usage.system_ns, elapsed);
        hart->itimer_system_ns += elapsed;
    }
}

void usage_of_thread(const thread_t* restrict thread, usage_t* restrict out)
{
    // With interrupts on, a switch could charge the current run between the load and the mark
    bool intr_state = disable_interrupts();

    load_usage(thread, out);

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

void usage_of_process(process_t* restrict process, usage_t* restrict out)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    *out = process->usage;

    // Under the lock, as process_set_vmspace changes
    // it under the lock before it releases the old one
    if(process->vmspace) {
        size_t peak = rmap_peak_pages(process->vmspace->pmap);

        if(peak > out->peak_pages) {
            out->peak_pages = peak;
        }
    }

    for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
        usage_t thread_usage = {};
        usage_of_thread(thread, &thread_usage);
        usage_add(out, &thread_usage);
    }

    spinlock_release_intr(&process->lock, intr_state);
}

void usage_to_rusage(const usage_t* restrict usage, rusage_t* restrict out)
{
    memset(out, 0, sizeof(*out));
    timeval_from_nsec(&out->ru_utime, usage->user_ns);
    timeval_from_nsec(&out->ru_stime, usage->system_ns);

    out->ru_maxrss = usage->peak_pages * PAGE_SIZE / 1024; // In KiB
    out->ru_minflt = usage->minor_faults;
    out->ru_majflt = usage->major_faults;
    out->ru_inblock = usage->blocks_in;
    out->ru_oublock = usage->blocks_out;
    out->ru_nvcsw = usage->voluntary_switches;
    out->ru_nivcsw = usage->involuntary_switches;
    out->ru_nsignals = usage->signals;
}
