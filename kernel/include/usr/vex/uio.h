#ifndef USR_VEX_UIO_H
#define USR_VEX_UIO_H

#include <vex/types.h>

/* One buffer of a vectored read or write */
struct __k_iovec {
    __k_uintptr_t base;
    __k_size_t len;
};

#endif /* USR_VEX_UIO_H */
