#include <vex/address.h>
#include <vex/atomic.h>
#include <vex/capability.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/hart.h>
#include <vex/pgrp.h>
#include <vex/process.h>
#include <vex/rlimit.h>
#include <vex/sched.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/uaccess.h>

#include "syscall_internal.h"

#define AFFINITY_WORD_BITS 64
#define AFFINITY_WORDS     ((num_harts + AFFINITY_WORD_BITS - 1) / AFFINITY_WORD_BITS)
#define AFFINITY_BYTES     (AFFINITY_WORDS * sizeof(uint64_t))

typedef struct priority_walk_s priority_walk_t;

struct priority_walk_s {
    const thread_t* caller;
    int which;
    uid_t uid; // Of the user that PRIO_USER names
    int nice;  // The new value for a set, the lowest found for a get
    bool found;
    int error;
};

static bool owns_target(const thread_t* restrict caller, process_t* restrict target)
{
    credentials_t* target_cred = process_get_cred(target);
    bool owner = caller->credentials->euid == target_cred->uid || caller->credentials->euid == target_cred->euid;

    cred_decref(target_cred);

    return owner;
}

static bool may_change(const thread_t* restrict caller, process_t* restrict target)
{
    return owns_target(caller, target) || cred_capable(caller->credentials, CAP_SYS_NICE);
}

static bool walk_matches(priority_walk_t* restrict walk, process_t* restrict process)
{
    if(walk->which != PRIO_USER) {
        return true;
    }

    credentials_t* credentials = process_get_cred(process);
    bool matches = credentials->uid == walk->uid;

    cred_decref(credentials);

    return matches;
}

static void get_member(process_t* restrict process, void* restrict arg)
{
    priority_walk_t* walk = arg;

    if(!walk_matches(walk, process)) {
        return;
    }

    int nice = atomic_load_n(&process->nice, ATOMIC_RELAXED);

    walk->found = true;

    if(nice < walk->nice) {
        walk->nice = nice;
    }
}

// The lowest nice value that RLIMIT_NICE lets a process
// without CAP_SYS_NICE set: a limit of n allows (20 - n)
static int nice_floor(const thread_t* restrict caller)
{
    rlimit_t limit = process_get_rlimit(caller->process, RLIMIT_NICE);

    if(limit.rlim_cur > NICE_BIAS - NICE_MIN) {
        return NICE_MIN;
    }

    int allowed = limit.rlim_cur;

    return NICE_BIAS - allowed;
}

// A failure on one member does not stop the others; the call reports the last one
static void set_member(process_t* restrict process, void* restrict arg)
{
    priority_walk_t* walk = arg;

    if(!walk_matches(walk, process)) {
        return;
    }

    walk->found = true;

    bool capable = cred_capable(walk->caller->credentials, CAP_SYS_NICE);

    if(!capable && !owns_target(walk->caller, process)) {
        walk->error = -EPERM;
        return;
    }

    bool lowers = walk->nice < atomic_load_n(&process->nice, ATOMIC_RELAXED);

    if(!capable && lowers && walk->nice < nice_floor(walk->caller)) {
        walk->error = -EACCES;
        return;
    }

    atomic_store(&process->nice, walk->nice, ATOMIC_RELAXED);
}

static int walk_process(priority_walk_t* restrict walk, pid_t who, void (*callback)(process_t* restrict process, void* restrict arg))
{
    process_t* process = process_locate_or_self(walk->caller->process, who);

    if(process == nullptr) {
        return -ESRCH;
    }

    callback(process, walk);
    process_decref(process);

    return 0;
}

static int walk_group(priority_walk_t* restrict walk, pid_t who, void (*callback)(process_t* restrict process, void* restrict arg))
{
    pid_t pgid = who;

    if(pgid == 0) {
        pgid = process_get_pgid(walk->caller->process);
    }

    return process_group_for_each(pgid, callback, walk);
}

static void walk_user(priority_walk_t* restrict walk, uid_t who, void (*callback)(process_t* restrict process, void* restrict arg))
{
    walk->uid = who;

    if(walk->uid == 0) {
        walk->uid = walk->caller->credentials->uid;
    }

    process_for_each(callback, walk);
}

static int walk_targets(priority_walk_t* restrict walk, pid_t who, void (*callback)(process_t* restrict process, void* restrict arg))
{
    int retval = 0;

    switch(walk->which) {
        case PRIO_PROCESS:
            retval = walk_process(walk, who, callback);
            break;

        case PRIO_PGRP:
            retval = walk_group(walk, who, callback);
            break;

        case PRIO_USER:
            walk_user(walk, who, callback);
            break;

        default:
            return -EINVAL;
    }

    if(retval < 0) {
        return retval;
    }

    if(!walk->found) {
        return -ESRCH;
    }

    return 0;
}

static intptr_t sys_getpriority(int which, pid_t who)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    priority_walk_t walk = {};
    walk.caller = thread;
    walk.which = which;
    walk.nice = NICE_MAX + 1;

    int retval = walk_targets(&walk, who, &get_member);

    if(retval < 0) {
        return retval;
    }

    return NICE_BIAS - walk.nice;
}

define_syscall(sys_getpriority, SYS_GETPRIORITY, 2);

static intptr_t sys_setpriority(int which, pid_t who, intptr_t value)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    intptr_t nice = value;

    if(nice < NICE_MIN) {
        nice = NICE_MIN;
    }

    if(nice > NICE_MAX) {
        nice = NICE_MAX;
    }

    priority_walk_t walk = {};
    walk.caller = thread;
    walk.which = which;
    walk.nice = nice;

    int retval = walk_targets(&walk, who, &set_member);

    if(retval < 0) {
        return retval;
    }

    return walk.error;
}

define_syscall(sys_setpriority, SYS_SETPRIORITY, 3);

static intptr_t sys_getscheduler(pid_t pid)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(pid < 0) {
        return -EINVAL;
    }

    process_t* process = process_locate_or_self(thread->process, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    process_decref(process);

    return SCHED_OTHER;
}

define_syscall(sys_getscheduler, SYS_GETSCHEDULER, 1);

// The only policy is SCHED_OTHER, with priority 0. UNDONE: SCHED_FIFO and SCHED_RR
static intptr_t set_scheduling(pid_t pid, uintptr_t policy, user_addr_t param)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(pid < 0 || policy != SCHED_OTHER) {
        return -EINVAL;
    }

    sched_param_t parameters;
    ssize_t copied = user_copyin(&parameters, param, sizeof(parameters));

    if(copied < 0) {
        return copied;
    }

    if(parameters.sched_priority) {
        return -EINVAL;
    }

    process_t* process = process_locate_or_self(thread->process, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    intptr_t retval = 0;

    if(!may_change(thread, process)) {
        retval = -EPERM;
    }

    process_decref(process);

    return retval;
}

static intptr_t sys_setscheduler(pid_t pid, uintptr_t policy, user_addr_t param)
{
    return set_scheduling(pid, policy, param);
}

define_syscall(sys_setscheduler, SYS_SETSCHEDULER, 3);

static intptr_t sys_setschedparam(pid_t pid, user_addr_t param)
{
    return set_scheduling(pid, SCHED_OTHER, param);
}

define_syscall(sys_setschedparam, SYS_SETSCHEDPARAM, 2);

static intptr_t sys_getschedparam(pid_t pid, user_addr_t param)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(pid < 0) {
        return -EINVAL;
    }

    process_t* process = process_locate_or_self(thread->process, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    process_decref(process);

    sched_param_t parameters = {};
    ssize_t copied = user_copyout(param, &parameters, sizeof(parameters));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_getschedparam, SYS_GETSCHEDPARAM, 2);

static intptr_t sys_getschedrange(uintptr_t policy, user_addr_t range)
{
    if(policy != SCHED_OTHER) {
        return -EINVAL;
    }

    sched_range_t limits = {};
    ssize_t copied = user_copyout(range, &limits, sizeof(limits));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_getschedrange, SYS_GETSCHEDRANGE, 2);

static intptr_t sys_getschedinterval(pid_t pid, user_addr_t interval)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(pid < 0) {
        return -EINVAL;
    }

    process_t* process = process_locate_or_self(thread->process, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    uint64_t timeslice = sched_timeslice_for_nice(atomic_load_n(&process->nice, ATOMIC_RELAXED));

    process_decref(process);

    timespec_t slice;
    timespec_from_nsec(&slice, timeslice);

    ssize_t copied = user_copyout(interval, &slice, sizeof(slice));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_getschedinterval, SYS_GETSCHEDINTERVAL, 2);

static uint64_t online_word(size_t word)
{
    uint64_t bits = 0;

    for(size_t bit = 0; bit < AFFINITY_WORD_BITS; bit += 1) {
        size_t index = word * AFFINITY_WORD_BITS + bit;

        if(index < num_harts && hart_online(index)) {
            bits |= UINT64_C(1) << bit;
        }
    }

    return bits;
}

// The mask is in whole words, as Linux has it. The call returns the size of the mask, not zero
static intptr_t sys_getaffinity(pid_t pid, size_t size, user_addr_t mask)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(pid < 0 || size < AFFINITY_BYTES || size % sizeof(uint64_t)) {
        return -EINVAL;
    }

    process_t* process = process_locate_or_self(thread->process, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    process_decref(process);

    for(size_t word = 0; word < AFFINITY_WORDS; word += 1) {
        uint64_t bits = online_word(word);
        user_addr_t target = user_addr_add(mask, word * sizeof(bits));
        ssize_t copied = user_copyout(target, &bits, sizeof(bits));

        if(copied < 0) {
            return copied;
        }
    }

    return AFFINITY_BYTES;
}

define_syscall(sys_getaffinity, SYS_GETAFFINITY, 3);

// UNDONE: The scheduler keeps the thread off the harts that the mask leaves out
static intptr_t sys_setaffinity(pid_t pid, size_t size, user_addr_t mask)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(pid < 0) {
        return -EINVAL;
    }

    // A mask that is shorter than ours leaves the rest clear, and the bytes past ours do not matter
    bool allowed = false;

    for(size_t word = 0; word < AFFINITY_WORDS; word += 1) {
        uint64_t bits = 0;
        size_t offset = word * sizeof(bits);

        if(offset < size) {
            size_t chunk = size - offset;

            if(chunk > sizeof(bits)) {
                chunk = sizeof(bits);
            }

            user_addr_t source = user_addr_add(mask, offset);
            ssize_t copied = user_copyin(&bits, source, chunk);

            if(copied < 0) {
                return copied;
            }
        }

        if(bits & online_word(word)) {
            allowed = true;
        }
    }

    process_t* process = process_locate_or_self(thread->process, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    bool permitted = may_change(thread, process);

    process_decref(process);

    if(!permitted) {
        return -EPERM;
    }

    if(!allowed) {
        return -EINVAL;
    }

    return 0;
}

define_syscall(sys_setaffinity, SYS_SETAFFINITY, 3);
