#include <vex/errno.h>
#include <vex/futex.h>
#include <vex/pmap.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/vms.h>

#include "syscall_internal.h"

static intptr_t sys_thread_create(uintptr_t entry, uintptr_t stack_top, uintptr_t tcb, uintptr_t argument, uintptr_t clear_tid)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    uintptr_t user_end = pmap_user_end(process->vmspace->pmap);

    if(entry >= user_end || stack_top >= user_end || tcb >= user_end || clear_tid >= user_end) {
        return -EINVAL;
    }

    if(clear_tid % sizeof(uint32_t)) {
        return -EINVAL;
    }

    thread_t* created = thread_create_sibling(thread, entry, stack_top, argument, tcb);

    if(created == nullptr) {
        return -ENOMEM;
    }

    created->clear_tid = clear_tid;

    pid_t tid = created->tid; // The thread may be gone by the time sched_enqueue returns

    sched_enqueue(created);

    return tid;
}

define_syscall(sys_thread_create, SYS_THREAD_CREATE, 5);

static intptr_t __noreturn sys_thread_exit(void)
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

define_syscall(sys_thread_exit, SYS_THREAD_EXIT, 0);

static intptr_t sys_gettid(void)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    return thread->tid;
}

define_syscall(sys_gettid, SYS_GETTID, 0);

static intptr_t sys_tcb_set(uintptr_t tcb)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    // A kernel address is useless to userspace and may not even be canonical
    if(tcb >= pmap_user_end(thread->process->vmspace->pmap)) {
        return -EINVAL;
    }

    arch_thread_set_tcb(thread, tcb);

    return 0;
}

define_syscall(sys_tcb_set, SYS_TCB_SET, 1);

static intptr_t sys_yield(void)
{
    sched_yield();
    return 0;
}

define_syscall(sys_yield, SYS_YIELD, 0);
