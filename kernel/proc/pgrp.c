#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/mutex.h>
#include <vex/pgrp.h>
#include <vex/process.h>
#include <vex/signal.h>
#include <vex/slab.h>

#define PROCESS_GROUP_BUCKETS 64
#define ORPHAN_BATCH          8

// Guards the table, every group's members, every process's group and every session's
// tty; a mutex, as signals sent to a group's members may sleep. It nests outside process locks
static mutex_t process_group_lock = MUTEX_INIT;
static process_group_t* process_group_table[PROCESS_GROUP_BUCKETS];

static __always_inline __nodiscard size_t pgid_bucket(pid_t pgid)
{
    return pgid % PROCESS_GROUP_BUCKETS;
}

static session_t* session_create(pid_t sid)
{
    session_t* session = slab_alloc(sizeof(session_t));

    if(session) {
        session->sid = sid;
        session->refcount = 1;
    }

    return session;
}

// The group takes a reference of its own to the session
static process_group_t* group_create_locked(pid_t pgid, session_t* restrict session)
{
    process_group_t* group = slab_alloc(sizeof(process_group_t));

    if(group == nullptr) {
        return nullptr;
    }

    session_incref(session);

    group->pgid = pgid;
    group->refcount = 1;
    group->num_members = 0;
    group->session = session;
    group->members = nullptr;

    size_t bucket = pgid_bucket(pgid);
    group->hash_next = process_group_table[bucket];
    process_group_table[bucket] = group;

    return group;
}

static process_group_t* find_in_table(pid_t pgid)
{
    for(process_group_t* it = process_group_table[pgid_bucket(pgid)]; it; it = it->hash_next) {
        if(it->pgid == pgid) {
            return it;
        }
    }

    return nullptr;
}

static void unhash_locked(process_group_t* restrict group)
{
    process_group_t** link = &process_group_table[pgid_bucket(group->pgid)];

    while(*link != group) {
        link = &(*link)->hash_next;
    }

    *link = group->hash_next;
}

// The group's reference passes to the process
static void join_locked(process_t* restrict process, process_group_t* restrict group)
{
    process->group_next = group->members;
    group->members = process;
    group->num_members += 1;

    atomic_store(&process->group, group, ATOMIC_RELEASE);
}

// Hands back the process's reference to the group
static process_group_t* leave_locked(process_t* restrict process)
{
    process_group_t* group = process->group;
    process_t** link = &group->members;

    while(*link != process) {
        link = &(*link)->group_next;
    }

    *link = process->group_next;
    process->group_next = nullptr;
    group->num_members -= 1;

    if(group->num_members == 0) {
        unhash_locked(group);
    }

    return group;
}

// The process must be attached already. Hands back the process's reference to the group it left
static process_group_t* move_locked(process_t* restrict process, process_group_t* restrict group)
{
    process_group_t* old_group = leave_locked(process);

    process_group_incref(group);
    join_locked(process, group);

    return old_group;
}

static int check_movable_locked(process_t* restrict caller, process_t* restrict process, session_t* restrict session)
{
    process_group_t* group = process->group;

    if(group == nullptr) {
        return -ESRCH; // Destroyed since it was located
    }

    if(group->session != session) {
        return -EPERM;
    }

    if(process != caller && (atomic_load_n(&process->flags, ATOMIC_ACQUIRE) & PROCESS_DID_EXEC)) {
        return -EACCES;
    }

    if(process->pid == session->sid) {
        return -EPERM;
    }

    return 0;
}

// Hands out a new reference to the group, created when the id is the process's own
static int destination_locked(process_t* restrict process, pid_t pgid, session_t* restrict session, process_group_t** restrict out)
{
    process_group_t* group = find_in_table(pgid);

    if(group) {
        if(group->session != session) {
            return -EPERM;
        }

        process_group_incref(group);
        *out = group;
        return 0;
    }

    if(pgid != process->pid) {
        return -EPERM;
    }

    group = group_create_locked(pgid, session);

    if(group == nullptr) {
        return -ENOMEM;
    }

    *out = group;
    return 0;
}

void process_group_lock_acquire(void)
{
    mutex_acquire(&process_group_lock);
}

void process_group_lock_release(void)
{
    mutex_release(&process_group_lock);
}

process_group_t* process_group_find_locked(pid_t pgid)
{
    kassert(mutex_held(&process_group_lock));

    return find_in_table(pgid);
}

void process_group_signal_locked(process_group_t* restrict group, int sig)
{
    kassert(mutex_held(&process_group_lock));

    siginfo_t info = {};
    info.si_signo = sig;
    info.si_code = SI_KERNEL;

    for(process_t* it = group->members; it; it = it->group_next) {
        signal_send(it, 0, &info);
    }
}

bool process_group_orphaned_locked(const process_group_t* restrict group)
{
    kassert(mutex_held(&process_group_lock));

    for(process_t* member = group->members; member; member = member->group_next) {
        pid_t ppid = atomic_load_n(&member->ppid, ATOMIC_SEQ_CST);
        bool exited = atomic_load_n(&member->flags, ATOMIC_ACQUIRE) & PROCESS_EXITING;

        if(exited || ppid == INIT_PID || ppid == 0) {
            continue;
        }

        process_t* parent = process_locate(ppid);

        if(parent == nullptr) {
            continue;
        }

        // A parent on its way out hands its children to init
        process_group_t* parent_group = parent->group;
        bool leaving = atomic_load_n(&parent->flags, ATOMIC_ACQUIRE) & PROCESS_EXITING;
        bool outside = !leaving && parent_group != group && parent_group->session == group->session;

        process_decref(parent);

        if(outside) {
            return false;
        }
    }

    return true;
}

static bool has_stopped_member(const process_group_t* restrict group)
{
    for(process_t* member = group->members; member; member = member->group_next) {
        bool exiting = atomic_load_n(&member->flags, ATOMIC_ACQUIRE) & PROCESS_EXITING;
        bool intr_state = spinlock_acquire_intr(&member->lock);
        bool stopped = member->job.state == JOB_STOPPED && !exiting;
        spinlock_release_intr(&member->lock, intr_state);

        if(stopped) {
            return true;
        }
    }

    return false;
}

static void hang_up_orphaned_locked(process_group_t* restrict group)
{
    if(process_group_orphaned_locked(group) && has_stopped_member(group)) {
        process_group_signal_locked(group, SIGHUP);
        process_group_signal_locked(group, SIGCONT);
    }
}

// Whether a process of the first group that has a child or a parent in the second one ties it to the rest of the session
static bool ties_session(const process_group_t* restrict first, const process_group_t* restrict second)
{
    return first != second && first->session == second->session;
}

static process_group_t* parent_group_locked(const process_t* restrict process)
{
    process_t* parent = process_locate(atomic_load_n(&process->ppid, ATOMIC_SEQ_CST));

    if(parent == nullptr) {
        return nullptr;
    }

    process_group_t* group = parent->group;

    process_decref(parent);

    return group;
}

static bool in_batch(process_group_t* const* restrict batch, size_t count, const process_group_t* restrict group)
{
    for(size_t i = 0; i < count; ++i) {
        if(batch[i] == group) {
            return true;
        }
    }

    return false;
}

// The groups stay put while the lock is held, so the
// pointers need no references. The first group is the
// one that the process was in when it tied other groups to the session
static void check_orphans_locked(process_t* restrict process, process_group_t* restrict before)
{
    kassert(mutex_held(&process_group_lock));

    size_t skip = 0;
    bool more = true;

    // A group is hung up when the process cuts its tie
    // to the session by leaving, not whenever it is orphaned
    process_group_t* parent_group = parent_group_locked(process);

    if(parent_group && ties_session(parent_group, before)) {
        hang_up_orphaned_locked(before);
    }

    while(more) {
        process_group_t* batch[ORPHAN_BATCH];
        size_t count = 0;
        size_t visited = 0;
        size_t index = 0;

        bool intr_state = spinlock_acquire_intr(&process->lock);
        process_t* child = process->children;

        while(child && count < ORPHAN_BATCH) {
            if(index >= skip) {
                process_group_t* group = atomic_load_n(&child->group, ATOMIC_ACQUIRE);

                if(ties_session(before, group) && !in_batch(batch, count, group)) {
                    batch[count] = group;
                    count += 1;
                }

                visited += 1;
            }

            index += 1;
            child = child->sibling_next;
        }

        more = child != nullptr;
        spinlock_release_intr(&process->lock, intr_state);

        for(size_t i = 0; i < count; ++i) {
            hang_up_orphaned_locked(batch[i]);
        }

        skip += visited;
    }
}

void process_group_check_orphans(process_t* restrict process)
{
    mutex_acquire(&process_group_lock);
    check_orphans_locked(process, process->group);
    mutex_release(&process_group_lock);
}

void session_incref(session_t* session)
{
    atomic_fetch_add(&session->refcount, 1, ATOMIC_RELAXED);
}

void session_decref(session_t* session)
{
    if(1 == atomic_fetch_sub(&session->refcount, 1, ATOMIC_ACQ_REL)) {
        slab_free(session);
    }
}

int process_group_attach(process_t* restrict process, process_t* restrict parent)
{
    mutex_acquire(&process_group_lock);

    if(parent) {
        process_group_t* group = parent->group;

        process_group_incref(group);
        join_locked(process, group);

        mutex_release(&process_group_lock);
        return 0;
    }

    session_t* session = session_create(process->pid);
    process_group_t* group = nullptr;

    if(session) {
        group = group_create_locked(process->pid, session);
        session_decref(session);
    }

    if(group) {
        join_locked(process, group);
    }

    mutex_release(&process_group_lock);

    if(group == nullptr) {
        return -ENOMEM;
    }

    return 0;
}

void process_group_detach(process_t* process)
{
    if(atomic_load_n(&process->group, ATOMIC_ACQUIRE) == nullptr) {
        return;
    }

    mutex_acquire(&process_group_lock);
    process_group_t* group = leave_locked(process);
    atomic_store(&process->group, nullptr, ATOMIC_RELEASE);
    mutex_release(&process_group_lock);

    process_group_decref(group);
}

process_group_t* process_group_find(pid_t pgid)
{
    mutex_acquire(&process_group_lock);

    process_group_t* group = find_in_table(pgid);

    if(group) {
        process_group_incref(group);
    }

    mutex_release(&process_group_lock);

    return group;
}

process_group_t* process_get_group(process_t* process)
{
    mutex_acquire(&process_group_lock);

    process_group_t* group = process->group;
    process_group_incref(group);

    mutex_release(&process_group_lock);

    return group;
}

void process_group_incref(process_group_t* group)
{
    atomic_fetch_add(&group->refcount, 1, ATOMIC_RELAXED);
}

void process_group_decref(process_group_t* group)
{
    if(1 == atomic_fetch_sub(&group->refcount, 1, ATOMIC_ACQ_REL)) {
        session_decref(group->session);
        slab_free(group);
    }
}

pid_t process_get_pgid(process_t* process)
{
    mutex_acquire(&process_group_lock);

    process_group_t* group = process->group;
    pid_t pgid = -ESRCH;

    if(group) {
        pgid = group->pgid;
    }

    mutex_release(&process_group_lock);

    return pgid;
}

pid_t process_get_sid(process_t* process)
{
    mutex_acquire(&process_group_lock);

    process_group_t* group = process->group;
    pid_t sid = -ESRCH;

    if(group) {
        session_t* session = group->session;
        sid = session->sid;
    }

    mutex_release(&process_group_lock);

    return sid;
}

pid_t process_group_new_session(process_t* process)
{
    pid_t pid = process->pid;

    mutex_acquire(&process_group_lock);

    // Whether or not the process is in it, a group with
    // its id would clash with the one the session needs
    if(find_in_table(pid)) {
        mutex_release(&process_group_lock);
        return -EPERM;
    }

    session_t* session = session_create(pid);
    process_group_t* group = nullptr;

    if(session) {
        group = group_create_locked(pid, session);
        session_decref(session);
    }

    if(group) {
        process_group_t* old_group = move_locked(process, group);

        check_orphans_locked(process, old_group);
        process_group_decref(old_group);
        process_group_decref(group);
    }

    mutex_release(&process_group_lock);

    if(group == nullptr) {
        return -ENOMEM;
    }

    return pid;
}

int process_group_move(process_t* restrict caller, process_t* restrict process, pid_t pgid)
{
    if(process != caller && atomic_load_n(&process->ppid, ATOMIC_ACQUIRE) != caller->pid) {
        return -ESRCH;
    }

    mutex_acquire(&process_group_lock);

    session_t* session = caller->group->session;
    int retval = check_movable_locked(caller, process, session);
    process_group_t* group = nullptr;

    if(retval == 0) {
        retval = destination_locked(process, pgid, session, &group);
    }

    if(retval == 0) {
        if(group != process->group) {
            process_group_t* old_group = move_locked(process, group);

            check_orphans_locked(process, old_group);
            process_group_decref(old_group);
        }

        process_group_decref(group);
    }

    mutex_release(&process_group_lock);

    return retval;
}

int process_group_for_each(pid_t pgid, void (*callback)(process_t* restrict process, void* restrict arg), void* restrict arg)
{
    mutex_acquire(&process_group_lock);

    process_group_t* group = find_in_table(pgid);

    if(group == nullptr) {
        mutex_release(&process_group_lock);
        return -ESRCH;
    }

    for(process_t* it = group->members; it; it = it->group_next) {
        callback(it, arg);
    }

    mutex_release(&process_group_lock);

    return 0;
}
