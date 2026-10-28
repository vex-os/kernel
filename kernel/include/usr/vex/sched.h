#ifndef USR_VEX_SCHED_H
#define USR_VEX_SCHED_H

#include <vex/types.h>

#define __k_PRIO_PROCESS 0
#define __k_PRIO_PGRP    1
#define __k_PRIO_USER    2

/* Nice values run from __k_NICE_MIN to __k_NICE_MAX. The getpriority call
 * returns __k_NICE_BIAS minus the nice value, which is never negative, so
 * that a result is not taken for an error. The C library undoes it */
#define __k_NICE_MIN  (-20)
#define __k_NICE_MAX  19
#define __k_NICE_BIAS 20

#define __k_SCHED_OTHER 0
#define __k_SCHED_FIFO  1
#define __k_SCHED_RR    2
#define __k_SCHED_BATCH 3
#define __k_SCHED_IDLE  5

struct __k_sched_param {
    __k_int32_t sched_priority;
};

/* The lowest and the highest priority of a policy */
struct __k_sched_range {
    __k_int32_t min;
    __k_int32_t max;
};

#endif /* USR_VEX_SCHED_H */
