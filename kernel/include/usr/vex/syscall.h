#ifndef USR_VEX_SYSCALL_H
#define USR_VEX_SYSCALL_H

/* Each subsystem owns a block of 32 numbers starting at a multiple of 0x20;
 * a new call takes the next free number in its block. Number 0 stays invalid
 * so that a zeroed register fails with ENOSYS */

#define __k_SYS_EXIT             0x020
#define __k_SYS_FORK             0x021
#define __k_SYS_EXEC             0x022
#define __k_SYS_WAITPID          0x023
#define __k_SYS_GETPID           0x024
#define __k_SYS_GETPPID          0x025
#define __k_SYS_SETSID           0x026
#define __k_SYS_SETPGID          0x027
#define __k_SYS_GETPGID          0x028
#define __k_SYS_GETSID           0x029
#define __k_SYS_GETPRIORITY      0x02B
#define __k_SYS_SETPRIORITY      0x02C
#define __k_SYS_GETSCHEDULER     0x02D
#define __k_SYS_SETSCHEDULER     0x02E
#define __k_SYS_GETSCHEDPARAM    0x02F
#define __k_SYS_SETSCHEDPARAM    0x030
#define __k_SYS_GETSCHEDRANGE    0x031
#define __k_SYS_GETSCHEDINTERVAL 0x032
#define __k_SYS_GETAFFINITY      0x033
#define __k_SYS_SETAFFINITY      0x034
#define __k_SYS_WAITID           0x035

#define __k_SYS_THREAD_CREATE 0x040
#define __k_SYS_THREAD_EXIT   0x041
#define __k_SYS_GETTID        0x042
#define __k_SYS_TCB_SET       0x043
#define __k_SYS_YIELD         0x044
#define __k_SYS_FUTEX_WAIT    0x045
#define __k_SYS_FUTEX_WAKE    0x046

#define __k_SYS_KILL         0x060
#define __k_SYS_TGKILL       0x061
#define __k_SYS_SIGACTION    0x062
#define __k_SYS_SIGPROCMASK  0x063
#define __k_SYS_SIGPENDING   0x064
#define __k_SYS_SIGRETURN    0x065
#define __k_SYS_SIGALTSTACK  0x066
#define __k_SYS_SIGSUSPEND   0x067
#define __k_SYS_SIGTIMEDWAIT 0x068
#define __k_SYS_SETITIMER    0x06A

#define __k_SYS_GETRESUID 0x080
#define __k_SYS_SETRESUID 0x081
#define __k_SYS_GETRESGID 0x082
#define __k_SYS_SETRESGID 0x083
#define __k_SYS_GETGROUPS 0x084
#define __k_SYS_SETGROUPS 0x085
#define __k_SYS_GETUMASK  0x086
#define __k_SYS_SETUMASK  0x087
#define __k_SYS_CAPGET    0x088
#define __k_SYS_CAPSET    0x089

#define __k_SYS_MMAP       0x0A0
#define __k_SYS_MUNMAP     0x0A1
#define __k_SYS_MPROTECT   0x0A2
#define __k_SYS_MSYNC      0x0A3
#define __k_SYS_MADVISE    0x0A4
#define __k_SYS_MLOCK      0x0A5
#define __k_SYS_MUNLOCK    0x0A6
#define __k_SYS_MLOCKALL   0x0A7
#define __k_SYS_MUNLOCKALL 0x0A8

#define __k_SYS_OPENAT    0x0C0
#define __k_SYS_CLOSE     0x0C1
#define __k_SYS_READ      0x0C2
#define __k_SYS_WRITE     0x0C3
#define __k_SYS_SEEK      0x0C4
#define __k_SYS_IOCTL     0x0C5
#define __k_SYS_DUP       0x0C6
#define __k_SYS_FSTAT     0x0C9
#define __k_SYS_FTRUNCATE 0x0CA
#define __k_SYS_FSYNC     0x0CB
#define __k_SYS_FCNTL     0x0CC
#define __k_SYS_PREAD     0x0CD
#define __k_SYS_PWRITE    0x0CE
#define __k_SYS_READV     0x0CF
#define __k_SYS_WRITEV    0x0D0
#define __k_SYS_PREADV    0x0D1
#define __k_SYS_PWRITEV   0x0D2
#define __k_SYS_PIPE      0x0D3
#define __k_SYS_PPOLL     0x0D4
#define __k_SYS_FLOCK     0x0D5

#define __k_SYS_FSTATAT    0x0E0
#define __k_SYS_FCHMODAT   0x0E1
#define __k_SYS_FCHOWNAT   0x0E2
#define __k_SYS_UTIMENSAT  0x0E3
#define __k_SYS_MKDIRAT    0x0E4
#define __k_SYS_UNLINKAT   0x0E5
#define __k_SYS_RENAMEAT   0x0E6
#define __k_SYS_LINKAT     0x0E7
#define __k_SYS_SYMLINKAT  0x0E8
#define __k_SYS_READLINKAT 0x0E9
#define __k_SYS_GETDENTS   0x0EA
#define __k_SYS_GETCWD     0x0EB
#define __k_SYS_CHDIR      0x0EC
#define __k_SYS_FCHDIR     0x0ED
#define __k_SYS_FACCESSAT  0x0EE
#define __k_SYS_CHROOT     0x0EF

#define __k_SYS_GETXATTRAT    0x100
#define __k_SYS_SETXATTRAT    0x101
#define __k_SYS_LISTXATTRAT   0x102
#define __k_SYS_REMOVEXATTRAT 0x103
#define __k_SYS_MOUNT         0x104
#define __k_SYS_UMOUNT        0x105
#define __k_SYS_SYNC          0x106
#define __k_SYS_SYNCFS        0x107
#define __k_SYS_STATVFS       0x108

#define __k_SYS_UNAME       0x120
#define __k_SYS_GETTIME     0x121
#define __k_SYS_SETTIME     0x122
#define __k_SYS_SETHOSTNAME 0x123
#define __k_SYS_NANOSLEEP   0x124
#define __k_SYS_GETTIMERES  0x125
#define __k_SYS_SYSINFO     0x126
#define __k_SYS_GETRLIMIT   0x127
#define __k_SYS_SETRLIMIT   0x128
#define __k_SYS_GETRUSAGE   0x129

#endif /* USR_VEX_SYSCALL_H */
