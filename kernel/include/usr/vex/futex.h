#ifndef USR_VEX_FUTEX_H
#define USR_VEX_FUTEX_H

/* The word can be shared between processes.
 * Use it on MAP_SHARED memory only, and pass
 * it in both futex_wait and futex_wake calls */
#define __k_FUTEX_SHARED 0x0001

#endif /* USR_VEX_FUTEX_H */
