#ifndef USR_VEX_SYSINFO_H
#define USR_VEX_SYSINFO_H

#include <vex/types.h>

struct __k_sysinfo {
    __k_uint64_t uptime_ns;   /* The monotonic clock */
    __k_uint64_t total_pages; /* Of memory the kernel gives out; the pages of the first 16 MiB for DMA are not counted */
    __k_uint64_t free_pages;
    __k_uint32_t cpus_online;
    __k_uint32_t cpus_total; /* Present, online or not */
};

#endif /* USR_VEX_SYSINFO_H */
