#ifndef USR_VEX_POLL_H
#define USR_VEX_POLL_H

#include <vex/types.h>

#define __k_POLLIN     0x0001
#define __k_POLLPRI    0x0002
#define __k_POLLOUT    0x0004
#define __k_POLLERR    0x0008
#define __k_POLLHUP    0x0010
#define __k_POLLNVAL   0x0020
#define __k_POLLRDNORM 0x0040
#define __k_POLLRDBAND 0x0080
#define __k_POLLWRNORM 0x0100
#define __k_POLLWRBAND 0x0200
#define __k_POLLMSG    0x0400
#define __k_POLLRDHUP  0x2000

struct __k_pollfd {
    __k_int32_t fd; /* A negative descriptor is skipped and gets no events */
    __k_int16_t events;
    __k_int16_t revents;
};

#endif /* USR_VEX_POLL_H */
