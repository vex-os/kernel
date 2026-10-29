#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/file.h>
#include <vex/hart.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/panic.h>
#include <vex/path.h>
#include <vex/process.h>
#include <vex/string.h>
#include <vex/syscall.h>
#include <vex/thread.h>

static syscall_t* syscall_table;
static size_t syscall_table_size;

// Another thread of the process may have swapped
// its credentials since this one last entered the kernel
static void catch_up_cred(thread_t* restrict thread)
{
    process_t* process = thread->process;

    if(process == nullptr || atomic_load_n(&process->credentials, ATOMIC_ACQUIRE) == thread->credentials) {
        return;
    }

    credentials_t* old_cred = thread->credentials;
    thread->credentials = process_get_cred(process);
    cred_decref(old_cred);
}

void syscall_dispatch(hart_context_t* restrict regs)
{
    if(regs->syscall_number >= syscall_table_size) {
        regs->syscall_retval = -ENOSYS;
        return;
    }

    if(syscall_table[regs->syscall_number].function == nullptr) {
        regs->syscall_retval = -ENOSYS;
        return;
    }

    thread_t* thread = this_thread();
    thread->syscall_ctx = regs;

    catch_up_cred(thread);

    uintptr_t args[6];
    args[0] = regs->syscall_arg_1;
    args[1] = regs->syscall_arg_2;
    args[2] = regs->syscall_arg_3;
    args[3] = regs->syscall_arg_4;
    args[4] = regs->syscall_arg_5;
    args[5] = regs->syscall_arg_6;

    regs->syscall_retval = syscall_table[regs->syscall_number].function(args);
}

int syscall_dirfd(int dirfd, const char* restrict path, vnode_t** restrict out)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(path[0] == PATH_SEPARATOR) {
        *out = nullptr;
        return 0;
    }

    if(dirfd == AT_FDCWD) {
        *out = process_get_cwd(process);

        if(*out == nullptr) {
            return -ENOENT;
        }

        return 0;
    }

    file_t* file = process_fd_get(process, dirfd);

    if(file == nullptr) {
        return -EBADF;
    }

    vnode_t* node = file->node;

    if(node->type != VNODE_DIRECTORY) {
        file_close(file);
        return -ENOTDIR;
    }

    vnode_incref(node);
    file_close(file);

    *out = node;

    return 0;
}

int syscall_resolve_at(int dirfd, const char* restrict path, unsigned flags, vnode_t** restrict out)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(path[0] == 0 && 0 == (flags & AT_EMPTY_PATH)) {
        return -ENOENT;
    }

    if(path[0] == 0) {
        if(dirfd == AT_FDCWD) {
            *out = process_get_cwd(process);
            return *out ? 0 : -ENOENT;
        }

        file_t* file = process_fd_get(process, dirfd);

        if(file == nullptr) {
            return -EBADF;
        }

        *out = file->node;
        vnode_incref(*out);
        file_close(file);

        return 0;
    }

    vnode_t* base_dir;
    int retval = syscall_dirfd(dirfd, path, &base_dir);

    if(retval < 0) {
        return retval;
    }

    unsigned path_flags = 0;

    if(flags & AT_SYMLINK_NOFOLLOW) {
        path_flags |= PATH_NOFOLLOW;
    }

    retval = path_resolve_at(base_dir, path, path_flags, thread->credentials, out);

    if(base_dir) {
        vnode_decref(base_dir);
    }

    return retval;
}

intptr_t syscall_fd_get(uintptr_t fd, file_t** restrict out)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    file_t* file = process_fd_get(process, fd);

    if(file == nullptr) {
        return -EBADF;
    }

    *out = file;

    return 0;
}

static void init_syscall(void)
{
    if(&unsorted_syscalls_start[0] == &unsorted_syscalls_end[0]) {
        pr_alert("syscall: no system calls defined?!");
        return;
    }

    unsigned long max_number = 0;

    for(const syscall_t* it = unsorted_syscalls_start; it < unsorted_syscalls_end; it += 1) {
        if(it->number > max_number) {
            max_number = it->number;
        }
    }

    syscall_table_size = max_number + 1;
    syscall_table = kmalloc(syscall_table_size * sizeof(syscall_t));

    if(syscall_table == nullptr) {
        panic("syscall: out of memory");
    }

    for(const syscall_t* it = unsorted_syscalls_start; it < unsorted_syscalls_end; it += 1) {
        kassert(syscall_table[it->number].function == nullptr);
        kassert(syscall_table[it->number].name == nullptr);

        memcpy(&syscall_table[it->number], it, sizeof(syscall_t));
    }
}

initcall_bsp_syscall(init_syscall, INIT_FIRST);
