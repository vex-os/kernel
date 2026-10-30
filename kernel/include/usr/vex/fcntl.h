#ifndef USR_VEX_FCNTL_H
#define USR_VEX_FCNTL_H

#include <vex/types.h>

#define __k_O_RDONLY    ((__k_mode_t)(0x0000))
#define __k_O_WRONLY    ((__k_mode_t)(0x0001))
#define __k_O_RDWR      ((__k_mode_t)(0x0002))
#define __k_O_ACCMODE   ((__k_mode_t)(0x0003))
#define __k_O_CREAT     ((__k_mode_t)(0x0004))
#define __k_O_TRUNC     ((__k_mode_t)(0x0008))
#define __k_O_APPEND    ((__k_mode_t)(0x0010))
#define __k_O_EXCL      ((__k_mode_t)(0x0020))
#define __k_O_NONBLOCK  ((__k_mode_t)(0x0040))
#define __k_O_DIRECTORY ((__k_mode_t)(0x0080))
#define __k_O_NOFOLLOW  ((__k_mode_t)(0x0100))
#define __k_O_CLOEXEC   ((__k_mode_t)(0x0200))

#define __k_F_DUPFD         0
#define __k_F_GETFD         1
#define __k_F_SETFD         2
#define __k_F_GETFL         3
#define __k_F_SETFL         4
#define __k_F_DUPFD_CLOEXEC 5
#define __k_F_GETLK         6
#define __k_F_SETLK         7
#define __k_F_SETLKW        8
#define __k_F_OFD_GETLK     9
#define __k_F_OFD_SETLK     10
#define __k_F_OFD_SETLKW    11

#define __k_FD_CLOEXEC 0x0001

#define __k_F_RDLCK 0
#define __k_F_WRLCK 1
#define __k_F_UNLCK 2

/* Operations of flock: one of LOCK_SH, LOCK_EX
 * and LOCK_UN, with LOCK_NB if it may not wait */
#define __k_LOCK_SH 0x0001
#define __k_LOCK_EX 0x0002
#define __k_LOCK_NB 0x0004
#define __k_LOCK_UN 0x0008

/* Modes of fallocate. Mode 0 makes sure that the storage of the range exists, and makes the file longer if
 * the range ends past its end. With KEEP_SIZE the file keeps its length. The other modes of Linux (punching
 * a hole, zeroing, collapsing, inserting a range) do not exist */
#define __k_FALLOC_FL_KEEP_SIZE 0x01

#define __k_SEEK_SET 0
#define __k_SEEK_CUR 1
#define __k_SEEK_END 2

#define __k_F_OK 0x0000
#define __k_X_OK 0x0001
#define __k_W_OK 0x0002
#define __k_R_OK 0x0004

#define __k_AT_FDCWD            (-100)
#define __k_AT_SYMLINK_NOFOLLOW 0x0100
#define __k_AT_REMOVEDIR        0x0200
#define __k_AT_EMPTY_PATH       0x0400
#define __k_AT_SYMLINK_FOLLOW   0x0800
#define __k_AT_EACCESS          0x1000

/* A range of l_len bytes from l_start, which
 * l_whence counts from as lseek does. A zero
 * length runs to the end of the file and past
 * it, and a negative one ends before l_start */
struct __k_flock {
    __k_int16_t l_type;
    __k_int16_t l_whence;
    __k_off_t l_start;
    __k_off_t l_len;
    __k_pid_t l_pid; /* Of the process that holds the lock, for F_GETLK; -1 for a lock of an open file, and 0 into the F_OFD_* calls */
};

#endif /* USR_VEX_FCNTL_H */
