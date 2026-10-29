#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/mount.h>
#include <vex/pgrp.h>
#include <vex/process.h>
#include <vex/rlimit.h>
#include <vex/sched.h>
#include <vex/signal.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/vms.h>
#include <vex/vnode.h>

#define PROCTABLE_BUCKETS  1024
#define PROCESS_WALK_BATCH 16

#define PROCESS_SIZE_BUDGET 512

static_assert(sizeof(process_t) <= PROCESS_SIZE_BUDGET);

static process_t* proctable[PROCTABLE_BUCKETS];
static spinlock_t proctable_lock = SPINLOCK_INIT; // Taken with interrupts disabled: the scheduler looks up parents
static pid_t next_pid = INIT_PID;

event_t init_exited;

static __always_inline __nodiscard size_t pid_bucket(pid_t pid)
{
    return pid % PROCTABLE_BUCKETS;
}

// The number of descriptors the process may have, read
// before fd_lock is taken as the limit lives under the
// lock of the process. A limit lowered below the descriptors
// in use leaves them alone and only turns away new ones
static size_t fd_limit(process_t* restrict process)
{
    rlimit_t limit = process_get_rlimit(process, RLIMIT_NOFILE);

    if(limit.rlim_cur > CONFIG_MAX_FDS) {
        return CONFIG_MAX_FDS;
    }

    return limit.rlim_cur;
}

static int grow_fds_locked(process_t* process, size_t fd, size_t limit)
{
    if(fd < process->max_fds) {
        return 0;
    }

    if(fd >= limit) {
        return -EMFILE;
    }

    size_t new_max = process->max_fds ? process->max_fds * 2 : 4;

    while(new_max <= fd) {
        new_max *= 2;
    }

    if(new_max > limit) {
        new_max = limit;
    }

    fd_slot_t* grown = krealloc(process->fds, new_max * sizeof(fd_slot_t));

    if(grown == nullptr) {
        return -ENOMEM;
    }

    for(size_t i = process->max_fds; i < new_max; ++i) {
        grown[i].file = nullptr;
        grown[i].flags = 0;
    }

    process->fds = grown;
    process->max_fds = new_max;

    return 0;
}

static void fill_exit_note(pending_signal_t* restrict note, const process_t* restrict child)
{
    credentials_t* credentials = child->credentials;

    siginfo_t info = {};
    info.si_signo = SIGCHLD;
    info.si_code = CLD_EXITED;
    info.si_pid = child->pid;
    info.si_uid = credentials->uid;
    info.si_status = child->exit_code;

    if(child->exit_signal) {
        info.si_code = CLD_KILLED;
        info.si_status = child->exit_signal;
    }

    note->info = info;
}

static void notify_parent(process_t* restrict process)
{
    process_t* parent = process_locate(atomic_load_n(&process->ppid, ATOMIC_SEQ_CST));

    if(parent == nullptr) {
        return;
    }

    // Under the lock waiters check their children under,
    // as the zombie flag itself was set without it
    bool intr_state = spinlock_acquire_intr(&parent->lock);
    event_trigger(&parent->child_exited, false);
    spinlock_release_intr(&parent->lock, intr_state);

    // After the event, so a handler's waitpid finds the zombie
    pending_signal_t* note = process->exit_note;
    process->exit_note = nullptr;
    fill_exit_note(note, process);
    signal_send_node(parent, note);

    process_decref(parent);
}

// Children are moved before their zombie flags are
// checked while a dying child sets its flag before
// reading its parent: one side or the other always gets to init
static void reparent_children(process_t* restrict process)
{
    process_t* init = process_locate(INIT_PID);

    if(init == process) {
        process_decref(init);
        init = nullptr;
    }

    bool intr_state = spinlock_acquire_intr(&process->lock);

    if(init) {
        spinlock_acquire(&init->lock);
    }

    pending_signal_t* note = nullptr;
    process_t* last = nullptr;

    for(process_t* it = process->children; it; it = it->sibling_next) {
        atomic_store_n(&it->ppid, init ? INIT_PID : 0, ATOMIC_SEQ_CST);

        // A zombie's own SIGCHLD went to this dying process, so init gets one for
        // it here; SIGCHLDs merge while pending, so the first zombie stands for all
        if(note == nullptr && (atomic_load_n(&it->flags, ATOMIC_SEQ_CST) & PROCESS_ZOMBIE)) {
            note = process->adopt_note;
            process->adopt_note = nullptr;
            fill_exit_note(note, it);
        }

        last = it;
    }

    if(init && last) {
        last->sibling_next = init->children;

        if(init->children) {
            init->children->sibling_prev = last;
        }

        init->children = process->children;
    }

    process->children = nullptr;

    if(init) {
        spinlock_release(&init->lock);
    }

    spinlock_release_intr(&process->lock, intr_state);

    if(init) {
        if(note) {
            event_trigger(&init->child_exited, false);
            signal_send_node(init, note);
        }

        process_decref(init);
    }
}

// Closing may sleep in a filesystem, so this runs in the exiting
// thread rather than when the scheduler reaps the last thread
static void release_files(process_t* restrict process)
{
    process_set_cwd(process, nullptr);

    spinlock_acquire(&process->fd_lock);

    fd_slot_t* fds = process->fds;
    size_t max_fds = process->max_fds;

    process->fds = nullptr;
    process->max_fds = 0;
    process->num_fds = 0;

    spinlock_release(&process->fd_lock);

    for(size_t i = 0; i < max_fds; ++i) {
        file_close(fds[i].file);
    }

    kfree(fds);
}

static void begin_exit(process_t* restrict process, int exit_code, int exit_signal)
{
    if(atomic_fetch_or(&process->flags, PROCESS_EXITING, ATOMIC_ACQ_REL) & PROCESS_EXITING) {
        return;
    }

    pr_verbose("process[%lu]: exiting with exit_code=%d signal=%d", (unsigned long)(process->pid), exit_code, exit_signal);

    process->exit_code = exit_code;
    process->exit_signal = exit_signal;

    bool intr_state = spinlock_acquire_intr(&process->lock);

    for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
        atomic_fetch_or(&thread->pending_work, THREAD_WORK_EXIT, ATOMIC_RELEASE);

        sched_kick(thread);
    }

    job_wake_locked(process);

    spinlock_release_intr(&process->lock, intr_state);
}

pid_t process_alloc_id(void)
{
    return atomic_fetch_add(&next_pid, 1, ATOMIC_RELAXED);
}

void process_finish_exit(process_t* restrict process)
{
    uint8_t flags = atomic_load_n(&process->flags, ATOMIC_ACQUIRE);

    if(0 == (flags & PROCESS_EXITING)) {
        return;
    }

    if(atomic_fetch_or(&process->flags, PROCESS_EXIT_FINISHED, ATOMIC_ACQ_REL) & PROCESS_EXIT_FINISHED) {
        return;
    }

    process_group_check_orphans(process);

    // A zombie must not keep filesystems
    // busy until it is waited for
    release_files(process);
}

process_t* process_create(process_t* restrict parent)
{
    process_t* process = slab_alloc(sizeof(process_t));

    if(process == nullptr) {
        return nullptr;
    }

    process->vmspace = vms_create();

    if(process->vmspace == nullptr) {
        slab_free(process);
        return nullptr;
    }

    signal_actions_t* parent_actions = nullptr;

    if(parent) {
        parent_actions = parent->signal_actions;
    }

    if(parent) {
        process->rlimits = process_get_rlimits(parent);
    }
    else {
        process->rlimits = rlimits_create();
    }

    process->signal_actions = signal_actions_create(parent_actions);
    process->exit_note = slab_alloc(sizeof(pending_signal_t));
    process->adopt_note = slab_alloc(sizeof(pending_signal_t));

    bool notes_missing = process->exit_note == nullptr || process->adopt_note == nullptr;

    if(process->signal_actions == nullptr || process->rlimits == nullptr || notes_missing) {
        rlimits_decref(process->rlimits);
        slab_free(process->adopt_note);
        slab_free(process->exit_note);
        signal_actions_destroy(process->signal_actions);
        vms_release(process->vmspace);
        slab_free(process);
        return nullptr;
    }

    if(parent) {
        process->ppid = parent->pid;
    }

    process->pid = process_alloc_id();
    process->refcount = 1;
    process->lock = SPINLOCK_INIT;
    process->fd_lock = SPINLOCK_INIT;
    event_init(&process->child_exited);
    event_init(&process->job.resume);
    mutex_init(&process->credentials_lock);

    if(parent) {
        process->credentials = process_get_cred(parent);
        process->umask = atomic_load_n(&parent->umask, ATOMIC_RELAXED);
        process->nice = atomic_load_n(&parent->nice, ATOMIC_RELAXED);
        process->cwd = process_get_cwd(parent);
    }
    else {
        process->credentials = &kernel_credentials;
        cred_incref(&kernel_credentials);
        event_init(&init_exited); // Only init has no parent
        process->umask = S_IWGRP | S_IWOTH;
        process->cwd = nullptr;
    }

    if(process->cwd == nullptr) {
        process->cwd = vfs_get_root();
    }

    if(process->cwd == nullptr) {
        rlimits_decref(process->rlimits);
        cred_decref(process->credentials);
        slab_free(process->adopt_note);
        slab_free(process->exit_note);
        signal_actions_destroy(process->signal_actions);
        vms_release(process->vmspace);
        slab_free(process);
        return nullptr;
    }

    if(process_group_attach(process, parent) < 0) {
        vnode_decref(process->cwd);
        process_decref(process);
        return nullptr;
    }

    if(parent) {
        bool intr_state = spinlock_acquire_intr(&parent->lock);

        process->sibling_next = parent->children;

        if(parent->children) {
            parent->children->sibling_prev = process;
        }

        parent->children = process;

        spinlock_release_intr(&parent->lock, intr_state);
    }

    size_t bucket = pid_bucket(process->pid);

    bool intr_state = spinlock_acquire_intr(&proctable_lock);

    process->hash_next = proctable[bucket];
    proctable[bucket] = process;

    spinlock_release_intr(&proctable_lock, intr_state);

    return process;
}

process_t* process_locate(pid_t pid)
{
    size_t bucket = pid_bucket(pid);

    bool intr_state = spinlock_acquire_intr(&proctable_lock);

    for(process_t* it = proctable[bucket]; it; it = it->hash_next) {
        if(it->pid == pid) {
            process_incref(it);
            spinlock_release_intr(&proctable_lock, intr_state);
            return it;
        }
    }

    spinlock_release_intr(&proctable_lock, intr_state);

    return nullptr;
}

process_t* process_locate_or_self(process_t* restrict caller, pid_t pid)
{
    if(pid == 0) {
        process_incref(caller);
        return caller;
    }

    if(pid < 0) {
        return nullptr;
    }

    return process_locate(pid);
}

void process_for_each(void (*callback)(process_t* restrict process, void* restrict arg), void* restrict arg)
{
    process_t* batch[PROCESS_WALK_BATCH];

    for(size_t bucket = 0; bucket < PROCTABLE_BUCKETS; bucket += 1) {
        bool started = false;
        pid_t below = 0;
        size_t count;

        // A bucket holds descending pids, as a new pid is the largest yet and goes to its head.
        // The walk goes on below the last pid it saw, so that what was added or removed
        // meanwhile shifts nothing; the processes added meanwhile are left out
        do {
            count = 0;

            bool intr_state = spinlock_acquire_intr(&proctable_lock);
            process_t* it = proctable[bucket];

            while(it && count < PROCESS_WALK_BATCH) {
                if(!started || it->pid < below) {
                    process_incref(it);
                    batch[count] = it;
                    count += 1;
                }

                it = it->hash_next;
            }

            spinlock_release_intr(&proctable_lock, intr_state);

            if(count) {
                started = true;
                below = batch[count - 1]->pid;
            }

            for(size_t i = 0; i < count; ++i) {
                callback(batch[i], arg);
                process_decref(batch[i]);
            }
        } while(count == PROCESS_WALK_BATCH);
    }
}

void process_incref(process_t* process)
{
    atomic_fetch_add(&process->refcount, 1, ATOMIC_RELAXED);
}

void process_decref(process_t* process)
{
    if(1 == atomic_fetch_sub(&process->refcount, 1, ATOMIC_ACQ_REL)) {
        // May run from the scheduler: only interrupt-safe teardown here,
        // everything else was released by process_destroy already

        if(process->vmspace) {
            vms_release(process->vmspace); // Never got as far as running a thread
        }

        cred_decref(process->credentials);
        rlimits_decref(process->rlimits);
        signal_flush(process->sig_queue);
        slab_free(process->adopt_note);
        slab_free(process->exit_note);
        signal_actions_destroy(process->signal_actions);
        mutex_deinit(&process->credentials_lock);
        event_deinit(&process->job.resume);
        event_deinit(&process->child_exited);
        slab_free(process);
    }
}

void process_exit(process_t* process, int exit_code)
{
    begin_exit(process, exit_code, 0);
}

void process_kill(process_t* process, int sig)
{
    begin_exit(process, 0, sig);
}

void process_thread_exit(process_t* process, thread_t* thread)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    // Already going with the whole process or for another thread's exec,
    // which do their own counting; the last-thread rule is not for it
    if(atomic_load(&thread->pending_work, ATOMIC_ACQUIRE) & THREAD_WORK_EXIT) {
        spinlock_release_intr(&process->lock, intr_state);
        return;
    }

    process->num_live_threads -= 1;

    bool last = (process->num_live_threads == 0);

    // Marked under the lock so that signals no longer pick this thread
    atomic_fetch_or(&thread->pending_work, THREAD_WORK_EXIT, ATOMIC_RELEASE);

    spinlock_release_intr(&process->lock, intr_state);

    if(last) {
        process_exit(process, 0);
        return;
    }

    signal_retarget(process);
}

int process_dethread(process_t* process, thread_t* thread)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    if((atomic_load_n(&process->flags, ATOMIC_ACQUIRE) & PROCESS_EXITING) || process->exec_thread) {
        spinlock_release_intr(&process->lock, intr_state);
        return -EINTR;
    }

    process->exec_thread = thread;

    for(thread_t* other = process->threads; other; other = other->process_next) {
        if(other != thread) {
            atomic_fetch_or(&other->pending_work, THREAD_WORK_EXIT, ATOMIC_RELEASE);
            sched_kick(other);
        }
    }

    job_wake_locked(process);

    // Uninterruptible: an exit meanwhile takes the other threads too,
    // so the wakeup from process_thread_reaped comes either way
    while(process->num_threads > 1) {
        sched_dequeue(thread, false);
        spinlock_release_intr(&process->lock, intr_state);
        sched_yield();
        intr_state = spinlock_acquire_intr(&process->lock);
    }

    process->exec_thread = nullptr;

    bool exiting = atomic_load_n(&process->flags, ATOMIC_ACQUIRE) & PROCESS_EXITING;

    if(!exiting) {
        process->num_live_threads = 1;
        thread->tid = process->pid;
    }

    spinlock_release_intr(&process->lock, intr_state);

    if(exiting) {
        return -EINTR;
    }

    // A process-wide signal may have been meant for one of the threads gone
    signal_retarget(process);

    return 0;
}

bool process_has_thread(process_t* process, pid_t tid)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);
    bool found = false;

    for(thread_t* thread = process->threads; thread; thread = thread->process_next) {
        if(thread->tid == tid && 0 == (atomic_load(&thread->pending_work, ATOMIC_ACQUIRE) & THREAD_WORK_EXIT)) {
            found = true;
            break;
        }
    }

    spinlock_release_intr(&process->lock, intr_state);

    return found;
}

void process_thread_reaped(process_t* process, thread_t* thread)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    if(thread->process_prev) {
        thread->process_prev->process_next = thread->process_next;
    }
    else {
        process->threads = thread->process_next;
    }

    if(thread->process_next) {
        thread->process_next->process_prev = thread->process_prev;
    }

    process->num_threads -= 1;

    bool last = (process->num_threads == 0);
    thread_t* exec_thread = nullptr;

    if(process->num_threads == 1) {
        exec_thread = process->exec_thread;
    }

    spinlock_release_intr(&process->lock, intr_state);

    if(exec_thread) {
        sched_enqueue(exec_thread);
    }

    if(last) {
        // No thread of the process is on any hart anymore, so
        // nothing has its page tables loaded and they can go
        vms_release(process->vmspace);
        process->vmspace = nullptr;

        atomic_fetch_or(&process->flags, PROCESS_ZOMBIE, ATOMIC_SEQ_CST);

        if(process->pid == INIT_PID) {
            event_trigger(&init_exited, true);
        }

        notify_parent(process);
        reparent_children(process);
    }

    process_decref(process);
}

bool process_claim(process_t* process)
{
    if(0 == (atomic_load_n(&process->flags, ATOMIC_ACQUIRE) & PROCESS_ZOMBIE)) {
        return false;
    }

    uint8_t old_flags = atomic_fetch_or(&process->flags, PROCESS_COLLECTED, ATOMIC_ACQ_REL);

    return 0 == (old_flags & PROCESS_COLLECTED);
}

void process_destroy(process_t* process)
{
    size_t bucket = pid_bucket(process->pid);

    bool intr_state = spinlock_acquire_intr(&proctable_lock);

    process_t* prev = nullptr;

    for(process_t* it = proctable[bucket]; it; it = it->hash_next) {
        if(it == process) {
            if(prev) {
                prev->hash_next = it->hash_next;
            }
            else {
                proctable[bucket] = it->hash_next;
            }

            break;
        }

        prev = it;
    }

    spinlock_release_intr(&proctable_lock, intr_state);

    process_t* parent = process_locate(atomic_load_n(&process->ppid, ATOMIC_SEQ_CST));

    if(parent) {
        intr_state = spinlock_acquire_intr(&parent->lock);

        if(process->sibling_prev) {
            process->sibling_prev->sibling_next = process->sibling_next;
        }
        else {
            parent->children = process->sibling_next;
        }

        if(process->sibling_next) {
            process->sibling_next->sibling_prev = process->sibling_prev;
        }

        spinlock_release_intr(&parent->lock, intr_state);

        process_decref(parent);
    }

    release_files(process);
    process_group_detach(process);

    process_decref(process);
}

credentials_t* process_get_cred(process_t* process)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    credentials_t* credentials = process->credentials;
    cred_incref(credentials);

    spinlock_release_intr(&process->lock, intr_state);

    return credentials;
}

void process_set_cred(process_t* process, credentials_t* credentials)
{
    kassert(mutex_held(&process->credentials_lock));

    bool intr_state = spinlock_acquire_intr(&process->lock);

    credentials_t* old_cred = process->credentials;
    atomic_store(&process->credentials, credentials, ATOMIC_RELEASE);

    spinlock_release_intr(&process->lock, intr_state);

    cred_decref(old_cred);
}

vnode_t* process_get_cwd(process_t* process)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    vnode_t* cwd = process->cwd;

    if(cwd) {
        vnode_incref(cwd);
    }

    spinlock_release_intr(&process->lock, intr_state);

    return cwd;
}

void process_set_cwd(process_t* process, vnode_t* node)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    vnode_t* old_cwd = process->cwd;
    process->cwd = node;

    spinlock_release_intr(&process->lock, intr_state);

    if(old_cwd) {
        vnode_decref(old_cwd);
    }
}

file_t* process_fd_get(process_t* process, int fd)
{
    spinlock_acquire(&process->fd_lock);

    file_t* file = nullptr;

    if(fd >= 0 && (size_t)(fd) < process->max_fds) {
        file = file_dupe(process->fds[fd].file);
    }

    spinlock_release(&process->fd_lock);

    return file;
}

int process_fd_alloc(process_t* process, file_t* file, int min_fd, uint32_t fd_flags)
{
    size_t limit = fd_limit(process);
    bool beyond_limit = min_fd > 0 && min_fd >= limit; // A limit of zero turns every call away with EMFILE below

    if(min_fd < 0 || min_fd >= CONFIG_MAX_FDS || beyond_limit) {
        return -EINVAL;
    }

    spinlock_acquire(&process->fd_lock);

    for(size_t fd = min_fd; fd < process->max_fds && fd < limit; fd += 1) {
        if(process->fds[fd].file == nullptr) {
            process->fds[fd].file = file;
            process->fds[fd].flags = fd_flags;
            process->num_fds += 1;
            spinlock_release(&process->fd_lock);
            return fd;
        }
    }

    int fd = process->max_fds;

    if(fd < min_fd) {
        fd = min_fd;
    }

    int retval = grow_fds_locked(process, fd, limit);

    if(retval < 0) {
        spinlock_release(&process->fd_lock);
        return retval;
    }

    process->fds[fd].file = file;
    process->fds[fd].flags = fd_flags;
    process->num_fds += 1;

    spinlock_release(&process->fd_lock);

    return fd;
}

int process_fd_dup(process_t* process, int old_fd, int new_fd, uint32_t fd_flags)
{
    file_t* copy = process_fd_get(process, old_fd);

    if(copy == nullptr) {
        return -EBADF;
    }

    if(old_fd == new_fd) {
        file_close(copy);
        return new_fd;
    }

    if(new_fd < 0) {
        int fd = process_fd_alloc(process, copy, 0, fd_flags);

        if(fd < 0) {
            file_close(copy);
        }

        return fd;
    }

    // As POSIX has it, a descriptor past
    // the limit is a bad one rather than one too many
    size_t limit = fd_limit(process);

    if(new_fd >= limit) {
        file_close(copy);
        return -EBADF;
    }

    spinlock_acquire(&process->fd_lock);

    int retval = grow_fds_locked(process, new_fd, limit);

    if(retval < 0) {
        spinlock_release(&process->fd_lock);
        file_close(copy);
        return retval;
    }

    file_t* replaced = process->fds[new_fd].file;
    process->fds[new_fd].file = copy;
    process->fds[new_fd].flags = fd_flags;

    if(replaced == nullptr) {
        process->num_fds += 1;
    }

    spinlock_release(&process->fd_lock);

    // Closing may sleep in the filesystem
    // so it happens outside the lock
    file_close(replaced);

    return new_fd;
}

int process_fd_release(process_t* process, int fd)
{
    spinlock_acquire(&process->fd_lock);

    if(fd < 0 || (size_t)(fd) >= process->max_fds || process->fds[fd].file == nullptr) {
        spinlock_release(&process->fd_lock);
        return -EBADF;
    }

    file_t* file = process->fds[fd].file;
    process->fds[fd].file = nullptr;
    process->fds[fd].flags = 0;
    process->num_fds -= 1;

    spinlock_release(&process->fd_lock);

    file_close(file);

    return 0;
}

int process_fd_get_flags(process_t* process, int fd)
{
    spinlock_acquire(&process->fd_lock);

    int retval = -EBADF;

    if(fd >= 0 && (size_t)(fd) < process->max_fds && process->fds[fd].file) {
        retval = process->fds[fd].flags;
    }

    spinlock_release(&process->fd_lock);

    return retval;
}

int process_fd_set_flags(process_t* process, int fd, uint32_t fd_flags)
{
    spinlock_acquire(&process->fd_lock);

    int retval = -EBADF;

    if(fd >= 0 && (size_t)(fd) < process->max_fds && process->fds[fd].file) {
        process->fds[fd].flags = fd_flags;
        retval = 0;
    }

    spinlock_release(&process->fd_lock);

    return retval;
}

void process_fds_close_on_exec(process_t* process)
{
    size_t fd = 0;

    // Closing may sleep in the filesystem, so each
    // descriptor leaves the table under the lock and closes outside it
    for(;;) {
        spinlock_acquire(&process->fd_lock);

        file_t* file = nullptr;

        while(fd < process->max_fds && file == nullptr) {
            fd_slot_t* slot = &process->fds[fd];

            if(slot->file && (slot->flags & FD_CLOEXEC)) {
                file = slot->file;
                slot->file = nullptr;
                slot->flags = 0;
                process->num_fds -= 1;
            }

            fd += 1;
        }

        spinlock_release(&process->fd_lock);

        if(file == nullptr) {
            return;
        }

        file_close(file);
    }
}

int process_fds_copy(process_t* restrict process, process_t* restrict source)
{
    spinlock_acquire(&source->fd_lock);

    fd_slot_t* fds = nullptr;

    if(source->max_fds) {
        fds = kmalloc(source->max_fds * sizeof(fd_slot_t));

        if(fds == nullptr) {
            spinlock_release(&source->fd_lock);
            return -ENOMEM;
        }

        for(size_t i = 0; i < source->max_fds; ++i) {
            fds[i].file = file_dupe(source->fds[i].file);
            fds[i].flags = source->fds[i].flags;
        }
    }

    process->fds = fds;
    process->max_fds = source->max_fds;
    process->num_fds = source->num_fds;

    spinlock_release(&source->fd_lock);

    return 0;
}
