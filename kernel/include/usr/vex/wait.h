#ifndef USR_VEX_WAIT_H
#define USR_VEX_WAIT_H

#include <vex/types.h>

/* The options of waitpid */

#define __k_WNOHANG    0x0001
#define __k_WUNTRACED  0x0002 /* Report a child that stopped */
#define __k_WCONTINUED 0x0008 /* Report a child that continued */

/* The low seven bits hold the terminating signal, zero for a normal exit */

#define __k_W_EXITCODE(code, sig) (((__k_int32_t)((code) & 0xFF) << 8) | ((sig) & 0x7F))

#define __k_WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define __k_WTERMSIG(status)    ((status) & 0x7F)
#define __k_WIFEXITED(status)   (__k_WTERMSIG(status) == 0)
#define __k_WIFSIGNALED(status) (__k_WTERMSIG(status) && __k_WTERMSIG(status) != 0x7F)

/* A stopped child has 0x7F in the low byte and the signal that stopped it above, a continued one has all bits set */

#define __k_W_STOPCODE(sig) ((__k_int32_t)(((sig) & 0xFF) << 8) | 0x7F)
#define __k_W_CONTINUED     0xFFFF

#define __k_WIFSTOPPED(status)   (((status) & 0xFF) == 0x7F)
#define __k_WSTOPSIG(status)     (((status) >> 8) & 0xFF)
#define __k_WIFCONTINUED(status) ((status) == __k_W_CONTINUED)

#endif /* USR_VEX_WAIT_H */
