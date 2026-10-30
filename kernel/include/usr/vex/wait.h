#ifndef USR_VEX_WAIT_H
#define USR_VEX_WAIT_H

#include <vex/types.h>

/* The options of waitpid */

#define __k_WNOHANG    0x0001
#define __k_WUNTRACED  0x0002 /* Report a child that stopped */
#define __k_WCONTINUED 0x0008 /* Report a child that continued */

/* The options that only waitid takes. WSTOPPED is WUNTRACED under its other name */

#define __k_WSTOPPED 0x0002
#define __k_WEXITED  0x0004     /* Report a child that exited */
#define __k_WNOWAIT  0x01000000 /* Leave the child to be waited for again */

/* What waitid waits for */

#define __k_P_ALL  0
#define __k_P_PID  1
#define __k_P_PGID 2

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
