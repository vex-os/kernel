#ifndef USR_VEX_DIRENT_H
#define USR_VEX_DIRENT_H

#include <vex/types.h>

/* The values of d_type are the ones of Linux */

#define __k_DT_UNKNOWN 0
#define __k_DT_FIFO    1
#define __k_DT_CHR     2
#define __k_DT_DIR     4
#define __k_DT_BLK     6
#define __k_DT_REG     8
#define __k_DT_LNK     10
#define __k_DT_SOCK    12

/* A record in the getdents buffer ends after d_name,
 * padded to 8 bytes; d_reclen is the step to the next one.
 * The fields are in the order of struct linux_dirent64 */
struct __k_dirent {
    __k_ino_t d_ino;
    __k_uint64_t d_off; /* Cookie of the next entry */
    __k_uint16_t d_reclen;
    __k_uint8_t d_type;
    __k_uint8_t d_name[256];
};

#endif /* USR_VEX_DIRENT_H */
