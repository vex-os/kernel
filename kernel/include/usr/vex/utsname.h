#ifndef USR_VEX_UTSNAME_H
#define USR_VEX_UTSNAME_H

#ifndef __k_UNAME_SIZE
#define __k_UNAME_SIZE 256
#endif

struct __k_utsname {
    char sysname[__k_UNAME_SIZE];
    char nodename[__k_UNAME_SIZE];
    char release[__k_UNAME_SIZE];
    char version[__k_UNAME_SIZE];
    char machine[__k_UNAME_SIZE];
};

#endif /* USR_VEX_UTSNAME_H */
