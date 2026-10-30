#include <vex/address.h>
#include <vex/atomic.h>
#include <vex/binfmt.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/kmalloc.h>
#include <vex/limits.h>
#include <vex/page.h>
#include <vex/pgrp.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/resource.h>
#include <vex/rlimit.h>
#include <vex/sched.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/usage.h>
#include <vex/vms.h>
#include <vex/wait.h>

#include "syscall_internal.h"

typedef struct wait_result_s wait_result_t;

// What a wait found about a child. The code is a CLD_* value, and the status
// is the exit code, the signal that ended or stopped the child, or SIGCONT
struct wait_result_s {
    pid_t pid;
    uid_t uid;
    int code;
    int status;
    usage_t usage; // With the children that the child collected
};

static int copyin_string_array(user_addr_t array, const char** restrict out, char** restrict cursor, const char* restrict limit)
{
    size_t count = 0;

    for(; array.value; count += 1) {
        user_addr_t string;
        user_addr_t slot = user_addr_add(array, count * sizeof(uintptr_t));
        ssize_t copied = user_copyin(&string, slot, sizeof(uintptr_t));

        if(copied < 0) {
            return copied;
        }

        if(string.value == 0) {
            break;
        }

        if(count == BINFMT_ARGS_MAX || *cursor >= limit) {
            return -E2BIG;
        }

        copied = user_copyin_str(*cursor, string, limit - *cursor);

        if(copied == -ENAMETOOLONG) {
            return -E2BIG;
        }

        if(copied < 0) {
            return copied;
        }

        out[count] = *cursor;
        *cursor += copied + 1;
    }

    out[count] = nullptr;

    return 0;
}

static int read_file_caps(vnode_t* restrict node, file_caps_t* restrict out, bool* restrict has_caps)
{
    vfs_cap_data_t data;
    ssize_t size = vnode_get_caps(node, &data, sizeof(data));

    *has_caps = false;

    if(size == -ENODATA || size == -EOPNOTSUPP) {
        return 0;
    }

    // Larger than any revision
    if(size == -ERANGE) {
        return -EINVAL;
    }

    if(size < 0) {
        return size;
    }

    int retval = cred_parse_file_caps(&data, size, out);

    if(retval == -ENODATA) {
        return 0;
    }

    if(retval < 0) {
        return retval;
    }

    *has_caps = true;

    return 0;
}

static int exec_cred(const credentials_t* restrict credentials, const stat_t* restrict st, const file_caps_t* restrict file,
    credentials_t** restrict out)
{
    credentials_t* copy = cred_copy(credentials);

    if(copy == nullptr) {
        return -ENOMEM;
    }

    bool setuid = st->st_mode & S_ISUID;
    bool setgid = (st->st_mode & S_ISGID) && (st->st_mode & S_IXGRP);

    if(setuid) {
        copy->euid = st->st_uid;
    }

    if(setgid) {
        copy->egid = st->st_gid;
    }

    copy->suid = copy->euid;
    copy->sgid = copy->egid;

    int retval = cred_exec_caps(copy, setuid || setgid, file);

    if(retval < 0) {
        cred_decref(copy);
        return retval;
    }

    *out = copy;

    return 0;
}

static intptr_t exec_image(thread_t* restrict thread, char* restrict image_path, char* restrict strings, user_addr_t path, user_addr_t argv,
    user_addr_t envp)
{
    ssize_t copied = user_copyin_str(image_path, path, PATH_MAX);

    if(copied < 0) {
        return copied;
    }

    const char* arguments[BINFMT_ARGS_MAX + 1];
    const char* environment[BINFMT_ARGS_MAX + 1];
    char* cursor = strings;

    int retval = copyin_string_array(argv, arguments, &cursor, strings + PAGE_SIZE);

    if(retval == 0) {
        retval = copyin_string_array(envp, environment, &cursor, strings + PAGE_SIZE);
    }

    if(retval < 0) {
        return retval;
    }

    process_t* process = thread->process;
    const credentials_t* credentials = thread->credentials;

    vnode_t* node;
    retval = syscall_resolve_at(AT_FDCWD, image_path, 0, &node);

    if(retval < 0) {
        return retval;
    }

    if(node->type != VNODE_FILE) {
        vnode_decref(node);
        return -EACCES;
    }

    stat_t st;
    retval = vnode_access(node, credentials, X_OK);

    if(retval == 0) {
        retval = vnode_stat(node, &st);
    }

    file_caps_t file_caps;
    bool has_caps;

    if(retval == 0) {
        retval = read_file_caps(node, &file_caps, &has_caps);
    }

    if(retval < 0) {
        vnode_decref(node);
        return retval;
    }

    file_t* file;
    retval = file_open_node(node, O_RDONLY, credentials, &file);

    if(retval < 0) {
        return retval;
    }

    // Built aside, so a failure leaves
    // the caller running in its old image
    vmspace_t* vmspace = vms_create();

    if(vmspace == nullptr) {
        file_close(file);
        return -ENOMEM;
    }

    mutex_acquire(&process->credentials_lock);

    credentials_t* new_cred = nullptr;
    binfmt_entry_t entry;
    rlimit_t stack_limit = process_get_rlimit(process, RLIMIT_STACK);

    if(has_caps) {
        retval = exec_cred(process->credentials, &st, &file_caps, &new_cred);
    }
    else {
        retval = exec_cred(process->credentials, &st, nullptr, &new_cred);
    }

    if(retval == 0) {
        retval = load_binary(vmspace, file, arguments, environment, new_cred, stack_limit.rlim_cur, &entry);
    }

    mutex_release(&process->credentials_lock);

    file_close(file);

    // The other threads go only once the new image is in place, and not under the
    // credentials lock: one of them may be waiting for it and never get to exit
    if(retval == 0) {
        retval = process_dethread(process, thread);
    }

    if(retval < 0) {
        if(new_cred) {
            cred_decref(new_cred);
        }

        vms_release(vmspace);
        return retval;
    }

    mutex_acquire(&process->credentials_lock);
    process_set_cred(process, new_cred);
    mutex_release(&process->credentials_lock);

    process_set_vmspace(process, vmspace);
    arch_thread_exec(thread, vmspace, entry.address, entry.user_stack);
    signal_actions_exec(process->signal_actions);

    thread->sig_altstack = 0;
    thread->sig_altstack_size = 0;
    thread->clear_tid = 0; // An address in the image just left
    atomic_fetch_or(&process->flags, PROCESS_DID_EXEC, ATOMIC_RELEASE);

    process_fds_close_on_exec(process);

    return 0;
}

static void fill_exited(wait_result_t* restrict out, const process_t* restrict child)
{
    out->pid = child->pid;
    out->uid = child->credentials->uid;
    out->code = CLD_EXITED;
    out->status = child->exit_code;
    out->usage = child->usage;
    usage_add(&out->usage, &child->children_usage);

    if(child->exit_signal) {
        out->code = CLD_KILLED;
        out->status = child->exit_signal;
    }
}

// The times of a zombie no longer change, so they are read without its lock
static void collect_usage(process_t* restrict caller, const process_t* restrict target)
{
    bool intr_state = spinlock_acquire_intr(&caller->lock);
    usage_add(&caller->children_usage, &target->usage);
    usage_add(&caller->children_usage, &target->children_usage);
    spinlock_release_intr(&caller->lock, intr_state);
}

static intptr_t collect_child(process_t* restrict caller, process_t* restrict target, uintptr_t options, wait_result_t* restrict result)
{
    fill_exited(result, target);

    intptr_t retval = target->pid;

    if(0 == (options & WNOWAIT)) {
        collect_usage(caller, target);
        process_destroy(target);
    }

    process_decref(target);

    return retval;
}

// WNOWAIT only looks, and leaves the report for the next wait
static bool take_report(process_t* restrict child, unsigned bit, uintptr_t options)
{
    if(options & WNOWAIT) {
        return atomic_load_n(&child->job.report, ATOMIC_ACQUIRE) & bit;
    }

    return job_claim_report(child, bit);
}

// A child that stops or continues still runs threads, so its time is a snapshot
static void read_live_usage(process_t* restrict child, usage_t* restrict out)
{
    usage_of_process(child, out);

    bool intr_state = spinlock_acquire_intr(&child->lock);
    usage_add(out, &child->children_usage);
    spinlock_release_intr(&child->lock, intr_state);

    process_decref(child);
}

static bool take_job_report(process_t* restrict child, uintptr_t options, wait_result_t* restrict out)
{
    bool stopped = (options & WSTOPPED) && take_report(child, JOB_REPORT_STOPPED, options);
    bool continued = !stopped && (options & WCONTINUED) && take_report(child, JOB_REPORT_CONTINUED, options);

    if(!stopped && !continued) {
        return false;
    }

    out->pid = child->pid;
    out->uid = child->credentials->uid;
    out->code = CLD_CONTINUED;
    out->status = SIGCONT;

    if(stopped) {
        out->code = CLD_STOPPED;
        out->status = child->job.stop_signal;
    }

    return true;
}

// A zombie is claimed, and so reaped, only when the caller asks for exits and does not say WNOWAIT
static process_t* take_zombie(process_t* restrict child, uintptr_t options)
{
    if(0 == (options & WEXITED)) {
        return nullptr;
    }

    if(options & WNOWAIT) {
        if(0 == (atomic_load_n(&child->flags, ATOMIC_ACQUIRE) & PROCESS_ZOMBIE)) {
            return nullptr;
        }
    }
    else if(!process_claim(child)) {
        return nullptr;
    }

    process_incref(child);

    return child;
}

// A group to wait for is compared by address, which the
// caller's reference keeps from going to another group
static bool wait_matches(process_t* restrict child, pid_t pid, const process_group_t* restrict group)
{
    if(group) {
        return atomic_load_n(&child->group, ATOMIC_ACQUIRE) == group;
    }

    return pid == -1 || child->pid == pid;
}

// @retval 0 With WNOHANG, no child has anything to report
static intptr_t wait_child(process_t* restrict caller, pid_t pid, const process_group_t* restrict group, uintptr_t options,
    wait_result_t* restrict result)
{
    for(;;) {
        process_t* target = nullptr;
        bool has_match = false;
        process_t* reporter = nullptr;

        bool intr_state = spinlock_acquire_intr(&caller->lock);

        for(process_t* it = caller->children; it; it = it->sibling_next) {
            // Another waiter has it, and it won't trigger child_exited again
            if(atomic_load_n(&it->flags, ATOMIC_ACQUIRE) & PROCESS_COLLECTED) {
                continue;
            }

            if(!wait_matches(it, pid, group)) {
                continue;
            }

            has_match = true;
            target = take_zombie(it, options);

            if(target) {
                break;
            }

            if(take_job_report(it, options, result)) {
                reporter = it;
                process_incref(reporter);
                break;
            }
        }

        if(reporter) {
            // The child is read after the lock of the caller goes, so the two locks are never held together
            spinlock_release_intr(&caller->lock, intr_state);
            read_live_usage(reporter, &result->usage);
            return result->pid;
        }

        if(target == nullptr && has_match) {
            if(options & WNOHANG) {
                spinlock_release_intr(&caller->lock, intr_state);
                return 0;
            }

            event_t* child_exited = &caller->child_exited;
            ssize_t result_wait = event_await_unlock(&child_exited, 1, &caller->lock, intr_state, 0);

            if(result_wait < 0) {
                return result_wait;
            }

            continue;
        }

        spinlock_release_intr(&caller->lock, intr_state);

        if(target == nullptr) {
            return -ECHILD;
        }

        return collect_child(caller, target, options, result);
    }
}

// Waits as waitpid and waitid say, once the pid is a pid, -1, zero for the group of the caller, or minus a group
static intptr_t wait_selected(process_t* restrict caller, pid_t pid, uintptr_t options, wait_result_t* restrict result)
{
    if(pid > 0 || pid == -1) {
        return wait_child(caller, pid, nullptr, options, result);
    }

    if(pid == INT32_MIN) {
        return -ECHILD; // No group has the negated id
    }

    process_group_t* group;

    if(pid == 0) {
        group = process_get_group(caller);
    }
    else {
        group = process_group_find(-pid);
    }

    if(group == nullptr) {
        return -ECHILD;
    }

    intptr_t retval = wait_child(caller, pid, group, options, result);
    process_group_decref(group);

    return retval;
}

static int status_of(const wait_result_t* restrict result)
{
    switch(result->code) {
        case CLD_STOPPED:
            return W_STOPCODE(result->status);

        case CLD_CONTINUED:
            return W_CONTINUED;

        case CLD_KILLED:
            return W_EXITCODE(0, result->status);

        default:
            return W_EXITCODE(result->status, 0);
    }
}

static intptr_t __noreturn sys_exit(int code)
{
    thread_t* thread = this_thread();

    if(thread->process) {
        process_exit(thread->process, code);
    }

    sched_terminate();
    unreachable();
}

define_syscall(sys_exit, SYS_EXIT, 1);

static intptr_t sys_fork(void)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    process_t* child = process_create(process);

    if(child == nullptr) {
        return -ENOMEM;
    }

    int retval = vms_clone(child->vmspace, process->vmspace);

    if(retval == 0) {
        retval = process_fds_copy(child, process);
    }

    thread_t* child_thread = nullptr;

    if(retval == 0) {
        child_thread = thread_create_fork(child, thread, thread->syscall_ctx);

        if(child_thread == nullptr) {
            retval = -ENOMEM;
        }
    }

    if(retval < 0) {
        process_destroy(child);
        return retval;
    }

    pid_t pid = child->pid;

    child_thread->sig_blocked = thread->sig_blocked;
    child_thread->sig_altstack = thread->sig_altstack;
    child_thread->sig_altstack_size = thread->sig_altstack_size;

    sched_enqueue(child_thread);

    return pid;
}

define_syscall(sys_fork, SYS_FORK, 0);

static intptr_t sys_exec(user_addr_t path, user_addr_t argv, user_addr_t envp)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    char* image_path = kmalloc(PATH_MAX);
    char* strings = pslab_alloc_virt();

    intptr_t retval = -ENOMEM;

    if(image_path && strings) {
        retval = exec_image(thread, image_path, strings, path, argv, envp);
    }

    pslab_free_virt(strings);
    kfree(image_path);

    return retval;
}

define_syscall(sys_exec, SYS_EXEC, 3);

static ssize_t copyout_rusage(user_addr_t address, const wait_result_t* restrict result)
{
    if(address.value == 0) {
        return 0;
    }

    rusage_t usage;
    usage_to_rusage(&result->usage, &usage);

    return user_copyout(address, &usage, sizeof(usage));
}

static intptr_t sys_waitpid(pid_t pid, user_addr_t wstatus, uintptr_t options, user_addr_t usage)
{
    if(options & ~(uintptr_t)(WNOHANG | WUNTRACED | WCONTINUED)) {
        return -EINVAL;
    }

    thread_t* thread = this_thread();
    process_t* caller = thread->process;

    if(caller == nullptr) {
        return -ECHILD;
    }

    wait_result_t result = {};
    intptr_t retval = wait_selected(caller, pid, options | WEXITED, &result);

    if(retval <= 0) {
        return retval;
    }

    if(wstatus.value) {
        int status = status_of(&result);
        ssize_t copied = user_copyout(wstatus, &status, sizeof(status));

        if(copied < 0) {
            return copied;
        }
    }

    ssize_t copied = copyout_rusage(usage, &result);

    if(copied < 0) {
        return copied;
    }

    return retval;
}

define_syscall(sys_waitpid, SYS_WAITPID, 4);

static pid_t wait_selector(unsigned idtype, pid_t id)
{
    switch(idtype) {
        case P_ALL:
            return -1;

        case P_PID:
            return id;

        case P_PGID:
            return -id;

        default:
            return INT32_MIN;
    }
}

// Zero in si_signo and si_pid says that WNOHANG found nothing, as POSIX has it
static intptr_t sys_waitid(unsigned idtype, pid_t id, user_addr_t info, uintptr_t options, user_addr_t usage)
{
    uintptr_t known = WNOHANG | WNOWAIT | WEXITED | WSTOPPED | WCONTINUED;

    if((options & ~known) || 0 == (options & (WEXITED | WSTOPPED | WCONTINUED))) {
        return -EINVAL;
    }

    if(idtype != P_ALL && idtype != P_PID && idtype != P_PGID) {
        return -EINVAL;
    }

    thread_t* thread = this_thread();
    process_t* caller = thread->process;

    if(caller == nullptr) {
        return -ECHILD;
    }

    // A group with the id zero is the one of the caller, and a process with it does not exist
    pid_t selector = wait_selector(idtype, id);

    if(id < 0 || (idtype == P_PID && id == 0)) {
        return -ECHILD;
    }

    wait_result_t result = {};
    intptr_t retval = wait_selected(caller, selector, options, &result);

    if(retval < 0) {
        return retval;
    }

    // Zero times when WNOHANG found nothing, as the result is still empty
    ssize_t copied = copyout_rusage(usage, &result);

    if(copied < 0) {
        return copied;
    }

    if(info.value == 0) {
        return 0;
    }

    siginfo_t reply = {};

    if(retval > 0) {
        reply.si_signo = SIGCHLD;
        reply.si_code = result.code;
        reply.si_pid = result.pid;
        reply.si_uid = result.uid;
        reply.si_status = result.status;
    }

    copied = user_copyout(info, &reply, sizeof(reply));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_waitid, SYS_WAITID, 5);

static intptr_t sys_getpid(void)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ESRCH;
    }

    return process->pid;
}

define_syscall(sys_getpid, SYS_GETPID, 0);

static intptr_t sys_getppid(void)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ESRCH;
    }

    return atomic_load_n(&process->ppid, ATOMIC_RELAXED);
}

define_syscall(sys_getppid, SYS_GETPPID, 0);

static intptr_t sys_setsid(void)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -EPERM;
    }

    return process_group_new_session(process);
}

define_syscall(sys_setsid, SYS_SETSID, 0);

static intptr_t sys_setpgid(pid_t pid, pid_t pgid)
{
    thread_t* thread = this_thread();
    process_t* caller = thread->process;

    if(caller == nullptr) {
        return -ESRCH;
    }

    if(pgid < 0) {
        return -EINVAL;
    }

    process_t* process = process_locate_or_self(caller, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    if(pgid == 0) {
        pgid = process->pid;
    }

    int retval = process_group_move(caller, process, pgid);
    process_decref(process);

    return retval;
}

define_syscall(sys_setpgid, SYS_SETPGID, 2);

static intptr_t sys_getpgid(pid_t pid)
{
    thread_t* thread = this_thread();
    process_t* caller = thread->process;

    if(caller == nullptr) {
        return -ESRCH;
    }

    process_t* process = process_locate_or_self(caller, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    pid_t pgid = process_get_pgid(process);
    process_decref(process);

    return pgid;
}

define_syscall(sys_getpgid, SYS_GETPGID, 1);

static intptr_t sys_getsid(pid_t pid)
{
    thread_t* thread = this_thread();
    process_t* caller = thread->process;

    if(caller == nullptr) {
        return -ESRCH;
    }

    process_t* process = process_locate_or_self(caller, pid);

    if(process == nullptr) {
        return -ESRCH;
    }

    pid_t sid = process_get_sid(process);
    process_decref(process);

    return sid;
}

define_syscall(sys_getsid, SYS_GETSID, 1);
