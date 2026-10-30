#ifndef USR_VEX_RESOURCE_H
#define USR_VEX_RESOURCE_H

#include <vex/time.h>
#include <vex/types.h>

/* The numbers are the ones Linux has */

#define __k_RLIMIT_CPU        0
#define __k_RLIMIT_FSIZE      1
#define __k_RLIMIT_DATA       2
#define __k_RLIMIT_STACK      3
#define __k_RLIMIT_CORE       4
#define __k_RLIMIT_RSS        5
#define __k_RLIMIT_NPROC      6
#define __k_RLIMIT_NOFILE     7
#define __k_RLIMIT_MEMLOCK    8
#define __k_RLIMIT_AS         9
#define __k_RLIMIT_LOCKS      10
#define __k_RLIMIT_SIGPENDING 11
#define __k_RLIMIT_MSGQUEUE   12
#define __k_RLIMIT_NICE       13
#define __k_RLIMIT_RTPRIO     14
#define __k_RLIMIT_RTTIME     15
#define __k_RLIMIT_NLIMITS    16

#define __k_RUSAGE_SELF     0
#define __k_RUSAGE_CHILDREN (-1)
#define __k_RUSAGE_THREAD   1

#define __k_RLIM_INFINITY (~(__k_uint64_t)(0))

struct __k_rlimit {
    __k_uint64_t rlim_cur; /* The soft limit, which the process may change up to rlim_max */
    __k_uint64_t rlim_max; /* The hard limit, which only a process with CAP_SYS_RESOURCE may raise */
};

struct __k_rusage {
    struct __k_timeval ru_utime;
    struct __k_timeval ru_stime;
    __k_int64_t ru_maxrss;
    __k_int64_t ru_ixrss;
    __k_int64_t ru_idrss;
    __k_int64_t ru_isrss;
    __k_int64_t ru_minflt;
    __k_int64_t ru_majflt;
    __k_int64_t ru_nswap;
    __k_int64_t ru_inblock;
    __k_int64_t ru_oublock;
    __k_int64_t ru_msgsnd;
    __k_int64_t ru_msgrcv;
    __k_int64_t ru_nsignals;
    __k_int64_t ru_nvcsw;
    __k_int64_t ru_nivcsw;
};

#endif /* USR_VEX_RESOURCE_H */
