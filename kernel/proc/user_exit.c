#include <vex/atomic.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/signal.h>
#include <vex/thread.h>

void exit_to_user(hart_context_t* restrict ctx)
{
    thread_t* thread = this_thread();

    for(;;) {
        uint32_t work = atomic_load(&thread->pending_work, ATOMIC_ACQUIRE);

        if(work == 0) {
            return;
        }

        enable_interrupts();

        if(work & THREAD_WORK_EXIT) {
            sched_terminate();
            unreachable();
        }

        // Parked first, as a stopped process takes no signal but the ones that kill it
        if(work & THREAD_WORK_STOP) {
            job_park(thread);
            disable_interrupts();
            continue;
        }

        if(work & THREAD_WORK_SIGNAL) {
            signal_deliver(ctx);
        }

        disable_interrupts();
    }
}
