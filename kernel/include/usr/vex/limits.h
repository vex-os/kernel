#ifndef USR_VEX_LIMITS_H
#define USR_VEX_LIMITS_H

#include <arch/limits.h>

#define __k_PATH_MAX __k_PAGE_SIZE
#define __k_PATH_LIM __k_PAGE_MAX

#define __k_NAME_MAX  255
#define __k_NAME_SIZE 256

#define __k_MAX_CANON 4096
#define __k_MAX_INPUT 4096

#define __k_IOV_MAX 1024

#endif /* USR_VEX_LIMITS_H */
