#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/page.h>
#include <vex/pgrp.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/sched.h>
#include <vex/signal.h>
#include <vex/slab.h>
#include <vex/string.h>
#include <vex/thread.h>

static_assert(sizeof(signal_actions_t) <= PAGE_SIZE);

enum {
    DEFAULT_TERM,
    DEFAULT_CORE,
    DEFAULT_IGNORE,
    DEFAULT_STOP,
    DEFAULT_CONT,
};

static const uint8_t default_actions[NSIG] = {
    [SIGQUIT - 1] = DEFAULT_CORE,
    [SIGILL - 1] = DEFAULT_CORE,
    [SIGTRAP - 1] = DEFAULT_CORE,
    [SIGABRT - 1] = DEFAULT_CORE,
    [SIGBUS - 1] = DEFAULT_CORE,
    [SIGFPE - 1] = DEFAULT_CORE,
    [SIGSEGV - 1] = DEFAULT_CORE,
    [SIGCHLD - 1] = DEFAULT_IGNORE,
    [SIGCONT - 1] = DEFAULT_CONT,
    [SIGSTOP - 1] = DEFAULT_STOP,
    [SIGTSTP - 1] = DEFAULT_STOP,
    [SIGTTIN - 1] = DEFAULT_STOP,
    [SIGTTOU - 1] = DEFAULT_STOP,
    [SIGURG - 1] = DEFAULT_IGNORE,
    [SIGXCPU - 1] = DEFAULT_CORE,
    [SIGXFSZ - 1] = DEFAULT_CORE,
    [SIGWINCH - 1] = DEFAULT_IGNORE,
    [SIGSYS - 1] = DEFAULT_CORE,
    [SIGINFO - 1] = DEFAULT_IGNORE,
};

static bool default_kills(int sig)
{
    uint8_t action = default_actions[sig - 1];
    return action == DEFAULT_TERM || action == DEFAULT_CORE;
}

static bool default_stops(int sig)
{
    return default_actions[sig - 1] == DEFAULT_STOP;
}

// Nothing is left to do for these once they are sent: a continue acts at send time
static bool default_ignores(int sig)
{
    uint8_t action = default_actions[sig - 1];
    return action == DEFAULT_IGNORE || action == DEFAULT_CONT;
}

static bool ignored_locked(const process_t* restrict process, int sig)
{
    signal_actions_t* signal_actions = process->signal_actions;
    sighandler_t handler = signal_actions->actions[sig - 1].sa_handler;

    if(handler == SIG_IGN) {
        return true;
    }

    // Init going down takes the system with it, so only signals it handles reach it;
    // its own faults are forced past this and still kill it
    if(handler == SIG_DFL && process->pid == INIT_PID) {
        return true;
    }

    return handler == SIG_DFL && default_ignores(sig);
}

static void queue_append(pending_signal_t** restrict queue, pending_signal_t* restrict node)
{
    while(*queue) {
        queue = &(*queue)->next;
    }

    node->next = nullptr;
    *queue = node;
}

static pending_signal_t* queue_take(pending_signal_t** restrict queue, int sig)
{
    for(; *queue; queue = &(*queue)->next) {
        pending_signal_t* node = *queue;

        if(node->info.si_signo == sig) {
            *queue = node->next;
            return node;
        }
    }

    return nullptr;
}

static void recalc_locked(thread_t* restrict thread)
{
    process_t* process = thread->process;
    sigset_t deliverable = (thread->sig_pending | process->sig_pending) & ~thread->sig_blocked;

    if(deliverable) {
        atomic_fetch_or(&thread->pending_work, THREAD_WORK_SIGNAL, ATOMIC_RELEASE);
    }
    else {
        atomic_fetch_and(&thread->pending_work, ~THREAD_WORK_SIGNAL, ATOMIC_RELEASE);
    }
}

static bool leaving(const thread_t* restrict thread)
{
    return atomic_load(&thread->pending_work, ATOMIC_ACQUIRE) & THREAD_WORK_EXIT;
}

static thread_t* choose_thread_locked(process_t* restrict process, sigset_t bit)
{
    thread_t* self = this_thread();

    if(self->process == process && 0 == (self->sig_blocked & bit) && !leaving(self)) {
        return self;
    }

    for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
        if(0 == (thread->sig_blocked & bit) && !leaving(thread)) {
            return thread;
        }
    }

    return nullptr;
}

static thread_t* find_thread_locked(process_t* restrict process, pid_t tid)
{
    for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
        if(thread->tid == tid && !leaving(thread)) {
            return thread;
        }
    }

    return nullptr;
}

static void drop_locked(sigset_t* restrict pending, pending_signal_t** restrict queue, int sig, pending_signal_t** restrict dropped)
{
    *pending &= ~sigbit(sig);

    for(pending_signal_t* node = queue_take(queue, sig); node; node = queue_take(queue, sig)) {
        node->next = *dropped;
        *dropped = node;
    }
}

// A target thread is looked up under the locks the signal is queued under,
// so it cannot be reaped in between; one already leaving counts as gone
#define STOP_SIGNALS (sigbit(SIGSTOP) | sigbit(SIGTSTP) | sigbit(SIGTTIN) | sigbit(SIGTTOU))

/// Drops what is pending of the signals in the mask, for the process and for each of its threads
/// @pre Both locks are held
static void purge_locked(process_t* restrict process, sigset_t mask, pending_signal_t** restrict dropped)
{
    for(int sig = 1; sig <= NSIG; sig += 1) {
        if(mask & sigbit(sig)) {
            drop_locked(&process->sig_pending, &process->sig_queue, sig, dropped);
        }
    }

    for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
        for(int sig = 1; sig <= NSIG; sig += 1) {
            if(mask & sigbit(sig)) {
                drop_locked(&thread->sig_pending, &thread->sig_queue, sig, dropped);
            }
        }

        recalc_locked(thread);
    }
}

// A continue acts when it is sent, whether it is blocked,
// ignored or caught; it and the stop signals cancel each other
static void job_effects(process_t* restrict process, int sig)
{
    bool stop = sigbit(sig) & STOP_SIGNALS;

    if(sig != SIGCONT && !stop) {
        return;
    }

    signal_actions_t* signal_actions = process->signal_actions;
    pending_signal_t* dropped = nullptr;
    bool continued = false;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);
    bool proc_intr_state = spinlock_acquire_intr(&process->lock);

    if(stop) {
        sigset_t continues = sigbit(SIGCONT);
        purge_locked(process, continues, &dropped);
    }
    else {
        purge_locked(process, STOP_SIGNALS, &dropped);
        continued = job_continue_locked(process);
    }

    spinlock_release_intr(&process->lock, proc_intr_state);
    spinlock_release_intr(&signal_actions->lock, intr_state);

    signal_flush(dropped);

    if(continued) {
        job_notify_continued(process);
    }
}

static int queue_signal(process_t* restrict process, pid_t tid, int sig, pending_signal_t* restrict node)
{
    job_effects(process, sig);

    signal_actions_t* signal_actions = process->signal_actions;
    sigset_t bit = sigbit(sig);

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);
    bool proc_intr_state = spinlock_acquire_intr(&process->lock);

    thread_t* target = nullptr;
    sigset_t* pending = &process->sig_pending;
    pending_signal_t** queue = &process->sig_queue;

    if(tid) {
        target = find_thread_locked(process, tid);

        if(target == nullptr) {
            spinlock_release_intr(&process->lock, proc_intr_state);
            spinlock_release_intr(&signal_actions->lock, intr_state);
            slab_free(node);
            return -ESRCH;
        }

        pending = &target->sig_pending;
        queue = &target->sig_queue;
    }

    if(*pending & bit) {
        spinlock_release_intr(&process->lock, proc_intr_state);
        spinlock_release_intr(&signal_actions->lock, intr_state);
        slab_free(node);
        return 0;
    }

    thread_t* thread = target;

    if(thread == nullptr) {
        thread = choose_thread_locked(process, bit);
    }

    if(thread && (thread->sig_blocked & bit)) {
        thread = nullptr;
    }

    // Only a signal some thread could take now is dropped as ignored; a blocked one
    // stays pending, its action may change before it is unblocked, and delivery checks again
    if(thread && ignored_locked(process, sig)) {
        spinlock_release_intr(&process->lock, proc_intr_state);
        spinlock_release_intr(&signal_actions->lock, intr_state);
        slab_free(node);
        return 0;
    }

    *pending |= bit;

    if(node) {
        queue_append(queue, node);
    }

    if(thread) {
        atomic_fetch_or(&thread->pending_work, THREAD_WORK_SIGNAL, ATOMIC_RELEASE);
        sched_kick(thread);
    }

    spinlock_release_intr(&process->lock, proc_intr_state);
    spinlock_release_intr(&signal_actions->lock, intr_state);

    return 0;
}

// Under process_group_lock, so that the exit of a parent either finds the
// process stopped and hangs its group up, or orphans the group before the check
static void stop_default(process_t* restrict process, int sig)
{
    process_group_lock_acquire();

    // POSIX: the signals of a terminal do not stop a process
    // of an orphaned group, as nobody could continue it
    bool from_terminal = sig != SIGSTOP;

    if(!from_terminal || !process_group_orphaned_locked(process->group)) {
        job_stop(process, sig);
    }

    process_group_lock_release();
}

static bool deliver(hart_context_t* restrict ctx, bool handlers)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;
    signal_actions_t* signal_actions = process->signal_actions;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);

    sigset_t* pending = &thread->sig_pending;
    pending_signal_t** queue = &thread->sig_queue;
    sigset_t deliverable = *pending & ~thread->sig_blocked;

    if(deliverable == 0) {
        pending = &process->sig_pending;
        queue = &process->sig_queue;
        deliverable = *pending & ~thread->sig_blocked;
    }

    if(deliverable == 0) {
        recalc_locked(thread);
        spinlock_release_intr(&signal_actions->lock, intr_state);
        signal_restore_mask(thread);
        return true;
    }

    int sig = __builtin_ctzll(deliverable) + 1;
    sigaction_t action = signal_actions->actions[sig - 1];
    bool caught = action.sa_handler != SIG_DFL && action.sa_handler != SIG_IGN;

    // Checked under the lock that the signal was chosen under, as one
    // that arrives after a check outside it would get a frame in mid-call
    if(caught && !handlers) {
        spinlock_release_intr(&signal_actions->lock, intr_state);
        return false;
    }

    *pending &= ~sigbit(sig);

    pending_signal_t* node = queue_take(queue, sig);
    sigset_t old_mask = thread->sig_blocked;

    // The handler returns to the mask from before
    // sigsuspend, which is not the one it runs with
    if(thread->sig_saved_valid) {
        old_mask = thread->sig_saved;
    }

    bool ignored = ignored_locked(process, sig); // It may have become so while it was blocked

    if(caught && (action.sa_flags & SA_RESETHAND)) {
        memset(&signal_actions->actions[sig - 1], 0, sizeof(sigaction_t));
    }

    recalc_locked(thread);

    spinlock_release_intr(&signal_actions->lock, intr_state);

    siginfo_t info = {};
    info.si_signo = sig;
    info.si_code = SI_KERNEL;

    if(node) {
        info = node->info;
        slab_free(node);
    }

    if(!caught) {
        // A process that stops in sigsuspend is still in it when it continues
        if(!default_stops(sig) || ignored) {
            signal_restore_mask(thread);
        }

        if(ignored) {
            return true;
        }

        if(default_stops(sig)) {
            stop_default(process, sig);
        }
        else {
            process_kill(process, sig);
        }

        return true;
    }

    if(thread->syscall_restart && 0 == (action.sa_flags & SA_RESTART)) {
        arch_syscall_interrupted(ctx);
    }

    thread->syscall_restart = false;
    thread->sig_saved_valid = false;

    // A stack that takes no frame leaves nothing to return to
    if(arch_signal_setup(thread, ctx, &action, &info, old_mask) < 0) {
        process_kill(process, SIGSEGV);
        return true;
    }

    usage_increase(&thread->usage.signals, 1);

    sigset_t blocked = action.sa_mask;

    if(0 == (action.sa_flags & SA_NODEFER)) {
        blocked |= sigbit(sig);
    }

    signal_mask(thread, SIG_BLOCK, blocked);

    return true;
}

signal_actions_t* signal_actions_create(signal_actions_t* restrict parent)
{
    signal_actions_t* signal_actions = pslab_alloc_virt();

    if(signal_actions == nullptr) {
        return nullptr;
    }

    // SIG_DFL with no flags and an empty mask is all zeroes
    memset(signal_actions, 0, sizeof(signal_actions_t));

    if(parent) {
        bool intr_state = spinlock_acquire_intr(&parent->lock);
        memcpy(signal_actions->actions, parent->actions, sizeof(signal_actions->actions));
        spinlock_release_intr(&parent->lock, intr_state);
    }

    signal_actions->lock = SPINLOCK_INIT;

    return signal_actions;
}

void signal_actions_destroy(signal_actions_t* restrict signal_actions)
{
    pslab_free_virt(signal_actions);
}

void signal_actions_exec(signal_actions_t* restrict signal_actions)
{
    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);

    for(int i = 0; i < NSIG; ++i) {
        sigaction_t* action = &signal_actions->actions[i];

        if(action->sa_handler == SIG_IGN) {
            action->sa_flags = 0;
            action->sa_mask = 0;
            continue;
        }

        memset(action, 0, sizeof(sigaction_t));
    }

    spinlock_release_intr(&signal_actions->lock, intr_state);
}

int signal_send(process_t* restrict process, pid_t tid, const siginfo_t* restrict info)
{
    if(info->si_signo == SIGKILL) {
        if(tid && !process_has_thread(process, tid)) {
            return -ESRCH;
        }

        if(process->pid != INIT_PID) {
            process_kill(process, SIGKILL); // Init is spared for the same reason as in ignored_locked
        }

        return 0;
    }

    // Allocated before the lock, under which nothing sleeps;
    // without it the signal still arrives, just with no siginfo
    pending_signal_t* node = slab_alloc(sizeof(pending_signal_t));

    if(node) {
        node->info = *info;
    }

    return queue_signal(process, tid, info->si_signo, node);
}

void signal_raise(process_t* restrict process, int sig)
{
    queue_signal(process, 0, sig, nullptr);
}

void signal_send_node(process_t* restrict process, pending_signal_t* restrict node)
{
    queue_signal(process, 0, node->info.si_signo, node);
}

void signal_retarget(process_t* restrict process)
{
    signal_actions_t* signal_actions = process->signal_actions;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);
    bool proc_intr_state = spinlock_acquire_intr(&process->lock);

    for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
        if(leaving(thread) || 0 == (process->sig_pending & ~thread->sig_blocked)) {
            continue;
        }

        atomic_fetch_or(&thread->pending_work, THREAD_WORK_SIGNAL, ATOMIC_RELEASE);
        sched_kick(thread);
    }

    spinlock_release_intr(&process->lock, proc_intr_state);
    spinlock_release_intr(&signal_actions->lock, intr_state);
}

void signal_force(thread_t* restrict thread, const siginfo_t* restrict info)
{
    process_t* process = thread->process;
    signal_actions_t* signal_actions = process->signal_actions;
    int sig = info->si_signo;
    sigset_t bit = sigbit(sig);
    pending_signal_t* dropped = nullptr;

    pending_signal_t* node = slab_alloc(sizeof(pending_signal_t));

    if(node) {
        node->info = *info;
    }

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);

    sigaction_t* action = &signal_actions->actions[sig - 1];

    // A blocked signal means its handler is the one that faulted, or the fault
    // came with the handler masked; running it would only fault again
    if(action->sa_handler == SIG_IGN || (thread->sig_blocked & bit)) {
        memset(action, 0, sizeof(sigaction_t));
    }

    // Killed right away rather than queued: delivery drops whatever init does not
    // handle, and a fault let through that way would only fault again
    bool fatal = action->sa_handler == SIG_DFL && default_kills(sig);

    if(!fatal) {
        thread->sig_blocked &= ~bit;

        // The fault's own siginfo replaces whatever was pending for it already
        drop_locked(&thread->sig_pending, &thread->sig_queue, sig, &dropped);
        thread->sig_pending |= bit;

        if(node) {
            queue_append(&thread->sig_queue, node);
            node = nullptr;
        }

        recalc_locked(thread);
    }

    spinlock_release_intr(&signal_actions->lock, intr_state);

    slab_free(node);
    signal_flush(dropped);

    if(fatal) {
        process_kill(process, sig);
    }
}

void signal_replace_mask(thread_t* restrict thread, sigset_t mask)
{
    if(!thread->sig_saved_valid) {
        thread->sig_saved = signal_mask(thread, SIG_SETMASK, mask);
        thread->sig_saved_valid = true;
    }
}

void signal_restore_mask(thread_t* restrict thread)
{
    if(thread->sig_saved_valid) {
        thread->sig_saved_valid = false;
        signal_mask(thread, SIG_SETMASK, thread->sig_saved);
    }
}

void signal_deliver(hart_context_t* restrict ctx)
{
    deliver(ctx, true);
}

bool signal_deliver_uncaught(hart_context_t* restrict ctx)
{
    return deliver(ctx, false);
}

void signal_action(process_t* restrict process, int sig, const sigaction_t* restrict act, sigaction_t* restrict old_action)
{
    signal_actions_t* signal_actions = process->signal_actions;
    pending_signal_t* dropped = nullptr;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);

    if(old_action) {
        *old_action = signal_actions->actions[sig - 1];
    }

    if(act) {
        signal_actions->actions[sig - 1] = *act;

        if(ignored_locked(process, sig)) {
            drop_locked(&process->sig_pending, &process->sig_queue, sig, &dropped);

            bool proc_intr_state = spinlock_acquire_intr(&process->lock);

            for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
                drop_locked(&thread->sig_pending, &thread->sig_queue, sig, &dropped);
            }

            spinlock_release_intr(&process->lock, proc_intr_state);
        }
    }

    spinlock_release_intr(&signal_actions->lock, intr_state);

    signal_flush(dropped);
}

sigset_t signal_mask(thread_t* restrict thread, int how, sigset_t set)
{
    process_t* process = thread->process;
    signal_actions_t* signal_actions = process->signal_actions;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);

    sigset_t old = thread->sig_blocked;
    sigset_t blocked = set;

    if(how == SIG_BLOCK) {
        blocked = old | set;
    }
    else if(how == SIG_UNBLOCK) {
        blocked = old & ~set;
    }

    thread->sig_blocked = blocked & ~(sigbit(SIGKILL) | sigbit(SIGSTOP));

    // Unblocking may leave something
    // deliverable on the way out
    recalc_locked(thread);

    spinlock_release_intr(&signal_actions->lock, intr_state);

    return old;
}

sigset_t signal_pending(thread_t* restrict thread)
{
    process_t* process = thread->process;
    signal_actions_t* signal_actions = process->signal_actions;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);
    sigset_t pending = (thread->sig_pending | process->sig_pending) & thread->sig_blocked;
    spinlock_release_intr(&signal_actions->lock, intr_state);

    return pending;
}

int signal_take(thread_t* restrict thread, sigset_t set, siginfo_t* restrict info)
{
    process_t* process = thread->process;
    signal_actions_t* signal_actions = process->signal_actions;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);

    sigset_t* pending = &thread->sig_pending;
    pending_signal_t** queue = &thread->sig_queue;
    sigset_t taken = *pending & set;

    if(taken == 0) {
        pending = &process->sig_pending;
        queue = &process->sig_queue;
        taken = *pending & set;
    }

    if(taken == 0) {
        spinlock_release_intr(&signal_actions->lock, intr_state);
        return 0;
    }

    int sig = __builtin_ctzll(taken) + 1;

    *pending &= ~sigbit(sig);

    pending_signal_t* node = queue_take(queue, sig);

    recalc_locked(thread);

    spinlock_release_intr(&signal_actions->lock, intr_state);

    memset(info, 0, sizeof(*info));
    info->si_signo = sig;
    info->si_code = SI_KERNEL;

    if(node) {
        *info = node->info;
        slab_free(node);
    }

    return sig;
}

bool signal_refused(thread_t* restrict thread, int sig)
{
    process_t* process = thread->process;
    signal_actions_t* signal_actions = process->signal_actions;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);
    bool ignored = ignored_locked(process, sig);
    spinlock_release_intr(&signal_actions->lock, intr_state);

    return ignored || (thread->sig_blocked & sigbit(sig));
}

bool signal_handler_pending(thread_t* restrict thread)
{
    process_t* process = thread->process;
    signal_actions_t* signal_actions = process->signal_actions;
    bool found = false;

    bool intr_state = spinlock_acquire_intr(&signal_actions->lock);
    sigset_t deliverable = (thread->sig_pending | process->sig_pending) & ~thread->sig_blocked;

    for(int sig = 1; sig <= NSIG && !found; sig += 1) {
        sighandler_t handler = signal_actions->actions[sig - 1].sa_handler;

        if((deliverable & sigbit(sig)) && handler != SIG_DFL && handler != SIG_IGN) {
            found = true;
        }
    }

    spinlock_release_intr(&signal_actions->lock, intr_state);

    return found;
}

bool signal_on_altstack(const thread_t* restrict thread, uintptr_t sp)
{
    uintptr_t base = thread->sig_altstack;

    return thread->sig_altstack_size && sp > base && sp - base <= thread->sig_altstack_size;
}

void signal_altstack_get(const thread_t* restrict thread, uintptr_t sp, stack_t* restrict out)
{
    out->ss_sp = (void*)(thread->sig_altstack);
    out->ss_size = thread->sig_altstack_size;
    out->ss_flags = 0;
    out->ss_reserved = 0;

    if(thread->sig_altstack_size == 0) {
        out->ss_flags = SS_DISABLE;
    }
    else if(signal_on_altstack(thread, sp)) {
        out->ss_flags = SS_ONSTACK;
    }
}

void signal_flush(pending_signal_t* restrict queue)
{
    while(queue) {
        pending_signal_t* next = queue->next;
        slab_free(queue);
        queue = next;
    }
}
