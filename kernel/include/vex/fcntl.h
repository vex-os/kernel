#pragma once

#include <usr/vex/fcntl.h>

#define O_RDONLY    __k_O_RDONLY
#define O_WRONLY    __k_O_WRONLY
#define O_RDWR      __k_O_RDWR
#define O_ACCMODE   __k_O_ACCMODE
#define O_CREAT     __k_O_CREAT
#define O_TRUNC     __k_O_TRUNC
#define O_APPEND    __k_O_APPEND
#define O_EXCL      __k_O_EXCL
#define O_NONBLOCK  __k_O_NONBLOCK
#define O_DIRECTORY __k_O_DIRECTORY
#define O_NOFOLLOW  __k_O_NOFOLLOW
#define O_CLOEXEC   __k_O_CLOEXEC

#define F_DUPFD         __k_F_DUPFD
#define F_GETFD         __k_F_GETFD
#define F_SETFD         __k_F_SETFD
#define F_GETFL         __k_F_GETFL
#define F_SETFL         __k_F_SETFL
#define F_DUPFD_CLOEXEC __k_F_DUPFD_CLOEXEC

#define FD_CLOEXEC __k_FD_CLOEXEC

#define F_OK __k_F_OK
#define X_OK __k_X_OK
#define W_OK __k_W_OK
#define R_OK __k_R_OK

#define AT_FDCWD            __k_AT_FDCWD
#define AT_SYMLINK_NOFOLLOW __k_AT_SYMLINK_NOFOLLOW
#define AT_REMOVEDIR        __k_AT_REMOVEDIR
#define AT_EMPTY_PATH       __k_AT_EMPTY_PATH
#define AT_SYMLINK_FOLLOW   __k_AT_SYMLINK_FOLLOW
