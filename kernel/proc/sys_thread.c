#include <vex/errno.h>
#include <vex/futex.h>
#include <vex/pmap.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/vms.h>

static intptr_t sys_thread_create(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    uintptr_t user_end = pmap_user_end(process->vmspace->pmap);

    if(args[0] >= user_end || args[1] >= user_end || args[2] >= user_end || args[4] >= user_end) {
        return -EINVAL;
    }

    if(args[4] % sizeof(uint32_t)) {
        return -EINVAL;
    }

    thread_t* created = thread_create_sibling(thread, args[0], args[1], args[3], args[2]);

    if(created == nullptr) {
        return -ENOMEM;
    }

    created->clear_tid = args[4];

    pid_t tid = created->tid; // The thread may be gone by the time sched_enqueue returns

    sched_enqueue(created);

    return tid;
}

define_syscall(sys_thread_create, SYS_THREAD_CREATE);

static intptr_t __noreturn sys_thread_exit(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        sched_terminate();
        unreachable();
    }

    // Out of the process first, so a joiner that sees the word cleared
    // no longer finds the thread; it may free the stack right then,
    // which is fine: nothing from here on touches user memory
    process_thread_exit(process, thread);

    if(thread->clear_tid) {
        uint32_t cleared = 0;

        if(user_copyout(thread->clear_tid, &cleared, sizeof(cleared)) >= 0) {
            futex_wake(process->vmspace, thread->clear_tid, SIZE_MAX, false);
            futex_wake(process->vmspace, thread->clear_tid, SIZE_MAX, true);
        }
    }

    sched_terminate();
    unreachable();
}

define_syscall(sys_thread_exit, SYS_THREAD_EXIT);

static intptr_t sys_gettid(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    return thread->tid;
}

define_syscall(sys_gettid, SYS_GETTID);

static intptr_t sys_tcb_set(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    // A kernel address is useless to userspace and may not even be canonical
    if(args[0] >= pmap_user_end(thread->process->vmspace->pmap)) {
        return -EINVAL;
    }

    arch_thread_set_tcb(thread, args[0]);

    return 0;
}

define_syscall(sys_tcb_set, SYS_TCB_SET);

static intptr_t sys_yield(const uintptr_t* restrict args)
{
    sched_yield();
    return 0;
}

define_syscall(sys_yield, SYS_YIELD);
