#pragma once

#include <usr/vex/resource.h>

#define RLIMIT_CPU        __k_RLIMIT_CPU
#define RLIMIT_FSIZE      __k_RLIMIT_FSIZE
#define RLIMIT_DATA       __k_RLIMIT_DATA
#define RLIMIT_STACK      __k_RLIMIT_STACK
#define RLIMIT_CORE       __k_RLIMIT_CORE
#define RLIMIT_RSS        __k_RLIMIT_RSS
#define RLIMIT_NPROC      __k_RLIMIT_NPROC
#define RLIMIT_NOFILE     __k_RLIMIT_NOFILE
#define RLIMIT_MEMLOCK    __k_RLIMIT_MEMLOCK
#define RLIMIT_AS         __k_RLIMIT_AS
#define RLIMIT_LOCKS      __k_RLIMIT_LOCKS
#define RLIMIT_SIGPENDING __k_RLIMIT_SIGPENDING
#define RLIMIT_MSGQUEUE   __k_RLIMIT_MSGQUEUE
#define RLIMIT_NICE       __k_RLIMIT_NICE
#define RLIMIT_RTPRIO     __k_RLIMIT_RTPRIO
#define RLIMIT_RTTIME     __k_RLIMIT_RTTIME
#define RLIMIT_NLIMITS    __k_RLIMIT_NLIMITS

#define RLIM_INFINITY __k_RLIM_INFINITY

#define RUSAGE_SELF     __k_RUSAGE_SELF
#define RUSAGE_CHILDREN __k_RUSAGE_CHILDREN
#define RUSAGE_THREAD   __k_RUSAGE_THREAD

typedef struct __k_rlimit rlimit_t;
typedef struct __k_rusage rusage_t;
