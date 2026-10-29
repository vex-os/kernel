#include <vex/atomic.h>
#include <vex/cred.h>
#include <vex/event.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/signal.h>
#include <vex/thread.h>

static bool leaving(const thread_t* restrict thread)
{
    return atomic_load(&thread->pending_work, ATOMIC_ACQUIRE) & THREAD_WORK_EXIT;
}

// The parent wakes for a waiter with WUNTRACED
// or WCONTINUED, and gets SIGCHLD unless it asked not to
static void notify_parent(process_t* restrict process, int code, int status)
{
    process_t* parent = process_locate(atomic_load_n(&process->ppid, ATOMIC_SEQ_CST));

    if(parent == nullptr) {
        return;
    }

    bool intr_state = spinlock_acquire_intr(&parent->lock);
    event_trigger(&parent->child_exited, false);
    spinlock_release_intr(&parent->lock, intr_state);

    signal_actions_t* signal_actions = parent->signal_actions;

    intr_state = spinlock_acquire_intr(&signal_actions->lock);
    unsigned flags = signal_actions->actions[SIGCHLD - 1].sa_flags;
    spinlock_release_intr(&signal_actions->lock, intr_state);

    if(0 == (flags & SA_NOCLDSTOP)) {
        // A reference, as another thread of
        // the process may change them meanwhile
        credentials_t* credentials = process_get_cred(process);

        siginfo_t info = {};
        info.si_signo = SIGCHLD;
        info.si_code = code;
        info.si_pid = process->pid;
        info.si_uid = credentials->uid;
        info.si_status = status;

        cred_decref(credentials);

        signal_send(parent, 0, &info);
    }

    process_decref(parent);
}

void job_stop(process_t* process, int sig)
{
    bool stopped = false;
    bool intr_state = spinlock_acquire_intr(&process->lock);

    bool exiting = atomic_load_n(&process->flags, ATOMIC_ACQUIRE) & PROCESS_EXITING;

    if(process->job.state == JOB_RUNNING && !exiting) {
        process->job.state = JOB_STOPPED;
        process->job.stop_signal = sig;
        atomic_fetch_and(&process->job.report, (uint8_t)(~JOB_REPORT_CONTINUED), ATOMIC_ACQ_REL);
        atomic_fetch_or(&process->job.report, JOB_REPORT_STOPPED, ATOMIC_ACQ_REL);
        stopped = true;

        for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
            if(!leaving(thread)) {
                atomic_fetch_or(&thread->pending_work, THREAD_WORK_STOP, ATOMIC_RELEASE);
                sched_kick(thread);
            }
        }
    }

    spinlock_release_intr(&process->lock, intr_state);

    if(stopped) {
        notify_parent(process, CLD_STOPPED, sig);
    }
}

bool job_continue_locked(process_t* process)
{
    if(process->job.state != JOB_STOPPED) {
        return false;
    }

    process->job.state = JOB_RUNNING;
    atomic_fetch_and(&process->job.report, (uint8_t)(~JOB_REPORT_STOPPED), ATOMIC_ACQ_REL);
    atomic_fetch_or(&process->job.report, JOB_REPORT_CONTINUED, ATOMIC_ACQ_REL);
    event_trigger(&process->job.resume, false);

    return true;
}

void job_notify_continued(process_t* process)
{
    notify_parent(process, CLD_CONTINUED, SIGCONT);
}

void job_wake_locked(process_t* process)
{
    event_trigger(&process->job.resume, false);
}

void job_park(thread_t* thread)
{
    process_t* process = thread->process;

    for(;;) {
        bool intr_state = spinlock_acquire_intr(&process->lock);

        if(process->job.state == JOB_RUNNING || leaving(thread)) {
            atomic_fetch_and(&thread->pending_work, ~THREAD_WORK_STOP, ATOMIC_RELEASE);
            spinlock_release_intr(&process->lock, intr_state);
            return;
        }

        // The trigger of a continue or an exit comes under the
        // same lock, so it can't pass between the check and the wait
        event_t* resume = &process->job.resume;
        event_await_blocking_unlock(&resume, 1, &process->lock, intr_state);
    }
}

bool job_resume_wait(thread_t* thread)
{
    hart_context_t* ctx = thread->syscall_ctx;

    for(;;) {
        uint32_t work = atomic_load(&thread->pending_work, ATOMIC_ACQUIRE);

        if((work & THREAD_WORK_EXIT) || signal_handler_pending(thread)) {
            return false;
        }

        // These signals only stop the
        // process, kill it or are dropped
        if(work & THREAD_WORK_SIGNAL) {
            if(!signal_deliver_uncaught(ctx)) {
                return false;
            }

            continue;
        }

        if(work & THREAD_WORK_STOP) {
            job_park(thread);
            continue;
        }

        return true;
    }
}

bool job_claim_report(process_t* process, unsigned bit)
{
    uint8_t before = atomic_fetch_and(&process->job.report, ~bit, ATOMIC_ACQ_REL);

    return before & bit;
}
