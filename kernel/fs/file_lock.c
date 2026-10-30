#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/fcntl.h>
#include <vex/file_lock.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/vnode.h>

// Linux stops at the same depth; a longer cycle
// goes unseen and sleeps as it would without the check
#define MAX_WAIT_CHAIN 10

typedef struct file_lock_s file_lock_t;
typedef struct lock_spares_s lock_spares_t;
typedef struct lock_waiter_s lock_waiter_t;

struct file_lock_s {
    file_lock_t* next;
    const void* owner;
    bool of_process;
    pid_t pid;
    unsigned type;
    off_t start;
    off_t end;
};

// The locks of one node, in no order
struct file_locks_s {
    spinlock_t lock;
    file_lock_t* head;
    event_t released; // Triggered after a range of an owner shrinks, goes or gets a weaker type
};

// A change takes at most two new ranges: one
// for the lock and one for the tail of an old
// lock that it splits. They are allocated before the spinlock is taken
struct lock_spares_s {
    file_lock_t* first;
    file_lock_t* second;
};

// A process that waits for a lock of another process,
// on the stack of its thread for as long as it sleeps
struct lock_waiter_s {
    lock_waiter_t* next;
    const void* owner;
    const void* blocker;
};

static_assert(sizeof(file_lock_t) <= SLAB_MAX_SIZE);
static_assert(sizeof(file_locks_t) <= SLAB_MAX_SIZE);

// Taken inside the spinlock of the locks
// of a node, never the other way around
static spinlock_t waiters_lock = SPINLOCK_INIT;
static lock_waiter_t* waiters = nullptr;

static file_locks_t* get_locks(vnode_t* restrict node, bool create)
{
    file_locks_t* locks = atomic_load(&node->locks, ATOMIC_ACQUIRE);

    if(locks || !create) {
        return locks;
    }

    file_locks_t* fresh = slab_alloc(sizeof(file_locks_t));

    if(fresh == nullptr) {
        return nullptr;
    }

    fresh->lock = SPINLOCK_INIT;
    event_init(&fresh->released);

    file_locks_t* expected = nullptr;

    if(atomic_cmpxchg(&node->locks, &expected, fresh, ATOMIC_ACQ_REL, ATOMIC_ACQUIRE)) {
        return fresh;
    }

    // Another thread made them first
    event_deinit(&fresh->released);
    slab_free(fresh);

    return expected;
}

static void free_list(file_lock_t* restrict head)
{
    while(head) {
        file_lock_t* next = head->next;
        slab_free(head);
        head = next;
    }
}

static bool take_spares(lock_spares_t* restrict spares)
{
    spares->first = slab_alloc(sizeof(file_lock_t));
    spares->second = slab_alloc(sizeof(file_lock_t));

    if(spares->first && spares->second) {
        return true;
    }

    slab_free(spares->first);
    slab_free(spares->second);

    return false;
}

static file_lock_t* use_spare(lock_spares_t* restrict spares)
{
    file_lock_t* spare = spares->first;

    if(spare) {
        spares->first = nullptr;
        return spare;
    }

    spare = spares->second;
    spares->second = nullptr;

    kassert(spare);

    return spare;
}

static void drop_spares(lock_spares_t* restrict spares)
{
    slab_free(spares->first);
    slab_free(spares->second);
}

static bool overlaps(const file_lock_t* restrict lock, off_t start, off_t end)
{
    return lock->start <= end && start <= lock->end;
}

static bool conflicts(const file_lock_t* restrict lock, const file_lock_request_t* restrict request)
{
    if(lock->owner == request->owner) {
        return false;
    }

    if(!overlaps(lock, request->start, request->end)) {
        return false;
    }

    return lock->type == F_WRLCK || request->type == F_WRLCK;
}

static file_lock_t* find_conflict_locked(const file_locks_t* restrict locks, const file_lock_request_t* restrict request)
{
    for(file_lock_t* lock = locks->head; lock; lock = lock->next) {
        if(conflicts(lock, request)) {
            return lock;
        }
    }

    return nullptr;
}

/// Takes the range out of the locks of the owner: a lock that covers both sides of it splits in two
/// @return Whether any lock of the owner shrank or went
static bool cut_range_locked(file_locks_t* restrict locks, const file_lock_request_t* restrict request, lock_spares_t* restrict spares,
    file_lock_t** restrict removed)
{
    bool cut = false;
    file_lock_t** prev = &locks->head;

    while(*prev) {
        file_lock_t* lock = *prev;

        if(lock->owner != request->owner || !overlaps(lock, request->start, request->end)) {
            prev = &lock->next;
            continue;
        }

        cut = true;

        bool keeps_head = lock->start < request->start;
        bool keeps_tail = lock->end > request->end;

        if(keeps_head && keeps_tail) {
            file_lock_t* tail = use_spare(spares);
            *tail = *lock;
            tail->start = request->end + 1;
            lock->end = request->start - 1;
            lock->next = tail;
            prev = &tail->next;
            continue;
        }

        if(keeps_head) {
            lock->end = request->start - 1;
            prev = &lock->next;
            continue;
        }

        if(keeps_tail) {
            lock->start = request->end + 1;
            prev = &lock->next;
            continue;
        }

        *prev = lock->next;
        lock->next = *removed;
        *removed = lock;
    }

    return cut;
}

static bool adjacent(const file_lock_t* restrict before, const file_lock_t* restrict after)
{
    return before->end < after->start && before->end + 1 == after->start;
}

// Joins the ranges of the owner with the same type that touch the new one, which overlap it no more
static void merge_neighbours_locked(file_locks_t* restrict locks, file_lock_t* restrict fresh, file_lock_t** restrict removed)
{
    file_lock_t** prev = &locks->head;

    while(*prev) {
        file_lock_t* lock = *prev;
        bool same = lock->owner == fresh->owner && lock->type == fresh->type;

        if(!same) {
            prev = &lock->next;
            continue;
        }

        if(adjacent(lock, fresh)) {
            fresh->start = lock->start;
        }
        else if(adjacent(fresh, lock)) {
            fresh->end = lock->end;
        }
        else {
            prev = &lock->next;
            continue;
        }

        *prev = lock->next;
        lock->next = *removed;
        *removed = lock;
    }
}

static void insert_locked(file_locks_t* restrict locks, const file_lock_request_t* restrict request, lock_spares_t* restrict spares,
    file_lock_t** restrict removed)
{
    file_lock_t* fresh = use_spare(spares);
    fresh->owner = request->owner;
    fresh->of_process = request->of_process;
    fresh->pid = request->pid;
    fresh->type = request->type;
    fresh->start = request->start;
    fresh->end = request->end;

    merge_neighbours_locked(locks, fresh, removed);

    fresh->next = locks->head;
    locks->head = fresh;
}

static bool holds_locked(const file_locks_t* restrict locks, const file_lock_request_t* restrict request)
{
    for(file_lock_t* lock = locks->head; lock; lock = lock->next) {
        bool same = lock->owner == request->owner && lock->type == request->type;
        bool covers = lock->start <= request->start && lock->end >= request->end;

        if(same && covers) {
            return true;
        }
    }

    return false;
}

static const void* blocker_of_locked(const void* owner)
{
    for(lock_waiter_t* waiter = waiters; waiter; waiter = waiter->next) {
        if(waiter->owner == owner) {
            return waiter->blocker;
        }
    }

    return nullptr;
}

// Follows the processes that wait for each other
// from the one in the way, looking for the caller
static bool closes_cycle_locked(const void* owner, const void* blocker)
{
    for(int depth = 0; depth < MAX_WAIT_CHAIN; ++depth) {
        if(blocker == owner) {
            return true;
        }

        blocker = blocker_of_locked(blocker);

        if(blocker == nullptr) {
            return false;
        }
    }

    return false;
}

// @retval false The wait would close a cycle, and nothing was added
static bool add_waiter(lock_waiter_t* restrict waiter, const file_lock_request_t* restrict request, const file_lock_t* restrict conflict)
{
    waiter->owner = request->owner;
    waiter->blocker = conflict->owner;

    spinlock_acquire(&waiters_lock);

    bool cycle = closes_cycle_locked(request->owner, conflict->owner);

    if(!cycle) {
        waiter->next = waiters;
        waiters = waiter;
    }

    spinlock_release(&waiters_lock);

    return !cycle;
}

static void remove_waiter(lock_waiter_t* restrict waiter)
{
    bool intr_state = spinlock_acquire_intr(&waiters_lock);

    for(lock_waiter_t** prev = &waiters; *prev; prev = &(*prev)->next) {
        if(*prev == waiter) {
            *prev = waiter->next;
            break;
        }
    }

    spinlock_release_intr(&waiters_lock, intr_state);
}

int file_lock_replace(vnode_t* restrict node, const file_lock_request_t* restrict request, bool wait)
{
    file_locks_t* locks = get_locks(node, false);

    if(locks) {
        bool intr_state = spinlock_acquire_intr(&locks->lock);
        bool held = holds_locked(locks, request);
        spinlock_release_intr(&locks->lock, intr_state);

        if(held) {
            return 0;
        }
    }

    file_lock_request_t unlock = *request;
    unlock.type = F_UNLCK;

    int retval = file_lock_set(node, &unlock, false);

    if(retval < 0 || request->type == F_UNLCK) {
        return retval;
    }

    return file_lock_set(node, request, wait);
}

int file_lock_set(vnode_t* restrict node, const file_lock_request_t* restrict request, bool wait)
{
    bool unlock = request->type == F_UNLCK;
    file_locks_t* locks = get_locks(node, !unlock);

    if(locks == nullptr && unlock) {
        return 0;
    }

    if(locks == nullptr) {
        return -ENOMEM;
    }

    lock_spares_t spares = {};

    if(!take_spares(&spares)) {
        return -ENOMEM;
    }

    bool intr_state = spinlock_acquire_intr(&locks->lock);

    for(;;) {
        file_lock_t* conflict = nullptr;

        if(!unlock) {
            conflict = find_conflict_locked(locks, request);
        }

        if(conflict == nullptr) {
            break;
        }

        if(!wait) {
            spinlock_release_intr(&locks->lock, intr_state);
            drop_spares(&spares);
            return -EAGAIN;
        }

        lock_waiter_t waiter = {};
        bool tracked = request->of_process && conflict->of_process;

        if(tracked && !add_waiter(&waiter, request, conflict)) {
            spinlock_release_intr(&locks->lock, intr_state);
            drop_spares(&spares);
            return -EDEADLK;
        }

        event_t* released = &locks->released;
        ssize_t result = event_await_unlock(&released, 1, &locks->lock, intr_state, 0);

        if(tracked) {
            remove_waiter(&waiter);
        }

        if(result < 0) {
            drop_spares(&spares);
            return result;
        }

        intr_state = spinlock_acquire_intr(&locks->lock);
    }

    file_lock_t* removed = nullptr;
    bool cut = cut_range_locked(locks, request, &spares, &removed);

    if(!unlock) {
        insert_locked(locks, request, &spares, &removed);
    }

    spinlock_release_intr(&locks->lock, intr_state);

    // A waiter registers under the spinlock before it
    // lets it go, so a trigger after it misses nobody
    if(cut) {
        event_trigger(&locks->released, false);
    }

    free_list(removed);
    drop_spares(&spares);

    return 0;
}

bool file_lock_test(vnode_t* restrict node, file_lock_request_t* restrict request)
{
    file_locks_t* locks = get_locks(node, false);

    if(locks == nullptr) {
        return false;
    }

    bool intr_state = spinlock_acquire_intr(&locks->lock);

    file_lock_t* conflict = find_conflict_locked(locks, request);

    if(conflict) {
        request->owner = conflict->owner;
        request->of_process = conflict->of_process;
        request->pid = conflict->pid;
        request->type = conflict->type;
        request->start = conflict->start;
        request->end = conflict->end;
    }

    spinlock_release_intr(&locks->lock, intr_state);

    return conflict;
}

void file_lock_release(vnode_t* restrict node, const void* restrict owner)
{
    file_locks_t* locks = get_locks(node, false);

    if(locks == nullptr) {
        return;
    }

    file_lock_t* removed = nullptr;
    bool intr_state = spinlock_acquire_intr(&locks->lock);

    file_lock_t** prev = &locks->head;

    while(*prev) {
        file_lock_t* lock = *prev;

        if(lock->owner != owner) {
            prev = &lock->next;
            continue;
        }

        *prev = lock->next;
        lock->next = removed;
        removed = lock;
    }

    spinlock_release_intr(&locks->lock, intr_state);

    if(removed) {
        event_trigger(&locks->released, false);
    }

    free_list(removed);
}

void file_locks_destroy(vnode_t* restrict node)
{
    file_locks_t* locks = node->locks;

    if(locks == nullptr) {
        return;
    }

    kassert(locks->head == nullptr);

    event_deinit(&locks->released);
    slab_free(locks);
}
