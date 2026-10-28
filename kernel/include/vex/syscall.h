#pragma once

#include <usr/vex/syscall.h>
#include <vex/compiler.h>
#include <vex/types.h>

typedef struct file_s file_t;
typedef struct hart_context_s hart_context_t;
typedef struct vnode_s vnode_t;

#define SYS_EXIT    __k_SYS_EXIT
#define SYS_FORK    __k_SYS_FORK
#define SYS_EXEC    __k_SYS_EXEC
#define SYS_WAITPID __k_SYS_WAITPID
#define SYS_GETPID  __k_SYS_GETPID
#define SYS_GETPPID __k_SYS_GETPPID
#define SYS_SETSID  __k_SYS_SETSID
#define SYS_SETPGID __k_SYS_SETPGID
#define SYS_GETPGID __k_SYS_GETPGID
#define SYS_GETSID  __k_SYS_GETSID

#define SYS_THREAD_CREATE __k_SYS_THREAD_CREATE
#define SYS_THREAD_EXIT   __k_SYS_THREAD_EXIT
#define SYS_GETTID        __k_SYS_GETTID
#define SYS_TCB_SET       __k_SYS_TCB_SET
#define SYS_YIELD         __k_SYS_YIELD
#define SYS_FUTEX_WAIT    __k_SYS_FUTEX_WAIT
#define SYS_FUTEX_WAKE    __k_SYS_FUTEX_WAKE

#define SYS_KILL        __k_SYS_KILL
#define SYS_TGKILL      __k_SYS_TGKILL
#define SYS_SIGACTION   __k_SYS_SIGACTION
#define SYS_SIGPROCMASK __k_SYS_SIGPROCMASK
#define SYS_SIGPENDING  __k_SYS_SIGPENDING
#define SYS_SIGRETURN   __k_SYS_SIGRETURN
#define SYS_SIGALTSTACK __k_SYS_SIGALTSTACK

#define SYS_GETRESUID __k_SYS_GETRESUID
#define SYS_SETRESUID __k_SYS_SETRESUID
#define SYS_GETRESGID __k_SYS_GETRESGID
#define SYS_SETRESGID __k_SYS_SETRESGID
#define SYS_GETGROUPS __k_SYS_GETGROUPS
#define SYS_SETGROUPS __k_SYS_SETGROUPS
#define SYS_GETUMASK  __k_SYS_GETUMASK
#define SYS_SETUMASK  __k_SYS_SETUMASK
#define SYS_CAPGET    __k_SYS_CAPGET
#define SYS_CAPSET    __k_SYS_CAPSET

#define SYS_MMAP     __k_SYS_MMAP
#define SYS_MUNMAP   __k_SYS_MUNMAP
#define SYS_MPROTECT __k_SYS_MPROTECT
#define SYS_MSYNC    __k_SYS_MSYNC

#define SYS_OPENAT    __k_SYS_OPENAT
#define SYS_CLOSE     __k_SYS_CLOSE
#define SYS_READ      __k_SYS_READ
#define SYS_WRITE     __k_SYS_WRITE
#define SYS_SEEK      __k_SYS_SEEK
#define SYS_IOCTL     __k_SYS_IOCTL
#define SYS_DUP       __k_SYS_DUP
#define SYS_DUP2      __k_SYS_DUP2
#define SYS_DUP3      __k_SYS_DUP3
#define SYS_FSTAT     __k_SYS_FSTAT
#define SYS_FTRUNCATE __k_SYS_FTRUNCATE
#define SYS_FSYNC     __k_SYS_FSYNC
#define SYS_FCNTL     __k_SYS_FCNTL
#define SYS_PREAD     __k_SYS_PREAD
#define SYS_PWRITE    __k_SYS_PWRITE

#define SYS_FSTATAT    __k_SYS_FSTATAT
#define SYS_FCHMODAT   __k_SYS_FCHMODAT
#define SYS_FCHOWNAT   __k_SYS_FCHOWNAT
#define SYS_UTIMENSAT  __k_SYS_UTIMENSAT
#define SYS_MKDIRAT    __k_SYS_MKDIRAT
#define SYS_UNLINKAT   __k_SYS_UNLINKAT
#define SYS_RENAMEAT   __k_SYS_RENAMEAT
#define SYS_LINKAT     __k_SYS_LINKAT
#define SYS_SYMLINKAT  __k_SYS_SYMLINKAT
#define SYS_READLINKAT __k_SYS_READLINKAT
#define SYS_GETDENTS   __k_SYS_GETDENTS
#define SYS_GETCWD     __k_SYS_GETCWD
#define SYS_CHDIR      __k_SYS_CHDIR
#define SYS_FCHDIR     __k_SYS_FCHDIR
#define SYS_FACCESSAT  __k_SYS_FACCESSAT

#define SYS_GETXATTRAT    __k_SYS_GETXATTRAT
#define SYS_SETXATTRAT    __k_SYS_SETXATTRAT
#define SYS_LISTXATTRAT   __k_SYS_LISTXATTRAT
#define SYS_REMOVEXATTRAT __k_SYS_REMOVEXATTRAT
#define SYS_MOUNT         __k_SYS_MOUNT
#define SYS_UMOUNT        __k_SYS_UMOUNT
#define SYS_SYNC          __k_SYS_SYNC
#define SYS_SYNCFS        __k_SYS_SYNCFS
#define SYS_STATVFS       __k_SYS_STATVFS

#define SYS_UNAME       __k_SYS_UNAME
#define SYS_GETTIME     __k_SYS_GETTIME
#define SYS_SETTIME     __k_SYS_SETTIME
#define SYS_SETHOSTNAME __k_SYS_SETHOSTNAME
#define SYS_NANOSLEEP   __k_SYS_NANOSLEEP
#define SYS_GETTIMERES  __k_SYS_GETTIMERES

typedef struct syscall_s syscall_t;

struct syscall_s {
    intptr_t (*function)(const uintptr_t* restrict args);
    const char* name;
    unsigned long number;
};

#define define_syscall(sc_function, sc_number)                                       \
    static const __used __section(".syscalls") syscall_t __syscall_##sc_function = { \
        .function = &(sc_function),                                                  \
        .name = #sc_function,                                                        \
        .number = (sc_number),                                                       \
    }

extern const syscall_t unsorted_syscalls_start[];
extern const syscall_t unsorted_syscalls_end[];

void syscall_dispatch(hart_context_t* restrict regs);

intptr_t syscall_with_fd(const uintptr_t* restrict args, intptr_t (*op)(file_t* restrict file, const uintptr_t* restrict args));
int syscall_dirfd(int dirfd, const char* restrict path, vnode_t** restrict out) __nodiscard;
int syscall_resolve_at(int dirfd, const char* restrict path, unsigned flags, vnode_t** restrict out) __nodiscard;
