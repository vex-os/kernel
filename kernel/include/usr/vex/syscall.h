#ifndef USR_VEX_SYSCALL_H
#define USR_VEX_SYSCALL_H

/* Each subsystem owns a block of 16 numbers starting at a multiple of 0x10;
 * a new call takes the next free number in its block. Number 0 stays invalid
 * so that a zeroed register fails with ENOSYS */

#define __k_SYS_EXIT    0x10
#define __k_SYS_FORK    0x11
#define __k_SYS_EXEC    0x12
#define __k_SYS_WAITPID 0x13
#define __k_SYS_GETPID  0x14
#define __k_SYS_GETPPID 0x15
#define __k_SYS_SETSID  0x16
#define __k_SYS_SETPGID 0x17
#define __k_SYS_GETPGID 0x18
#define __k_SYS_GETSID  0x19
#define __k_SYS_SLEEP   0x1A

#define __k_SYS_THREAD_CREATE 0x20
#define __k_SYS_THREAD_EXIT   0x21
#define __k_SYS_GETTID        0x22
#define __k_SYS_TCB_SET       0x23
#define __k_SYS_YIELD         0x24
#define __k_SYS_FUTEX_WAIT    0x25
#define __k_SYS_FUTEX_WAKE    0x26

#define __k_SYS_KILL        0x30
#define __k_SYS_TGKILL      0x31
#define __k_SYS_SIGACTION   0x32
#define __k_SYS_SIGPROCMASK 0x33
#define __k_SYS_SIGPENDING  0x34
#define __k_SYS_SIGRETURN   0x35
#define __k_SYS_SIGALTSTACK 0x36

#define __k_SYS_GETRESUID 0x40
#define __k_SYS_SETRESUID 0x41
#define __k_SYS_GETRESGID 0x42
#define __k_SYS_SETRESGID 0x43
#define __k_SYS_GETGROUPS 0x44
#define __k_SYS_SETGROUPS 0x45
#define __k_SYS_GETUMASK  0x46
#define __k_SYS_SETUMASK  0x47
#define __k_SYS_CAPGET    0x48
#define __k_SYS_CAPSET    0x49

#define __k_SYS_MMAP     0x50
#define __k_SYS_MUNMAP   0x51
#define __k_SYS_MPROTECT 0x52
#define __k_SYS_MSYNC    0x53

#define __k_SYS_OPENAT    0x60
#define __k_SYS_CLOSE     0x61
#define __k_SYS_READ      0x62
#define __k_SYS_WRITE     0x63
#define __k_SYS_SEEK      0x64
#define __k_SYS_IOCTL     0x65
#define __k_SYS_DUP       0x66
#define __k_SYS_DUP2      0x67
#define __k_SYS_DUP3      0x68
#define __k_SYS_FSTAT     0x69
#define __k_SYS_FTRUNCATE 0x6A
#define __k_SYS_FSYNC     0x6B
#define __k_SYS_FCNTL     0x6C

#define __k_SYS_FSTATAT    0x70
#define __k_SYS_FCHMODAT   0x71
#define __k_SYS_FCHOWNAT   0x72
#define __k_SYS_UTIMENSAT  0x73
#define __k_SYS_MKDIRAT    0x74
#define __k_SYS_UNLINKAT   0x75
#define __k_SYS_RENAMEAT   0x76
#define __k_SYS_LINKAT     0x77
#define __k_SYS_SYMLINKAT  0x78
#define __k_SYS_READLINKAT 0x79
#define __k_SYS_GETDENTS   0x7A
#define __k_SYS_GETCWD     0x7B
#define __k_SYS_CHDIR      0x7C
#define __k_SYS_FCHDIR     0x7D
#define __k_SYS_FACCESSAT  0x7E

#define __k_SYS_GETXATTRAT    0x80
#define __k_SYS_SETXATTRAT    0x81
#define __k_SYS_LISTXATTRAT   0x82
#define __k_SYS_REMOVEXATTRAT 0x83

#define __k_SYS_MOUNT   0x90
#define __k_SYS_UMOUNT  0x91
#define __k_SYS_SYNC    0x92
#define __k_SYS_SYNCFS  0x93
#define __k_SYS_STATVFS 0x94

#define __k_SYS_UNAME   0xA0
#define __k_SYS_GETTIME 0xA1
#define __k_SYS_SETTIME 0xA2

#endif /* USR_VEX_SYSCALL_H */
