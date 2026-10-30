#ifndef USR_VEX_TIME_H
#define USR_VEX_TIME_H

#include <vex/types.h>

#define __k_CLOCK_REALTIME           0
#define __k_CLOCK_MONOTONIC          1
#define __k_CLOCK_PROCESS_CPUTIME_ID 2
#define __k_CLOCK_THREAD_CPUTIME_ID  3
#define __k_CLOCK_MONOTONIC_RAW      4
#define __k_CLOCK_REALTIME_COARSE    5
#define __k_CLOCK_MONOTONIC_COARSE   6
#define __k_CLOCK_BOOTTIME           7

/* Flag of the nanosleep call: the request
 * is a point on the clock and not a duration */
#define __k_TIMER_ABSTIME 1

/* Timers of the setitimer call: wallclock time, user
 * CPU time, and user and system CPU time of the process */
#define __k_ITIMER_REAL    0
#define __k_ITIMER_VIRTUAL 1
#define __k_ITIMER_PROF    2

struct __k_timespec {
    __k_time_t tv_sec;
    __k_suseconds_t tv_nsec;
};

struct __k_timeval {
    __k_time_t tv_sec;
    __k_suseconds_t tv_usec;
};

struct __k_itimerspec {
    struct __k_timespec it_interval;
    struct __k_timespec it_value;
};

#endif /* USR_VEX_TIME_H */
