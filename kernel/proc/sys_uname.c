#include <vex/atomic.h>
#include <vex/capability.h>
#include <vex/clocksrc.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/hart.h>
#include <vex/hostname.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/strings.h>
#include <vex/syscall.h>
#include <vex/sysinfo.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/utsname.h>
#include <vex/version.h>

#include "syscall_internal.h"

static intptr_t sys_uname(user_addr_t buffer)
{
    utsname_t uname;
    kstrncpy(uname.sysname, k_sysname, sizeof(uname.sysname));
    kstrncpy(uname.release, k_release, sizeof(uname.release));
    kstrncpy(uname.version, k_version, sizeof(uname.version));
    kstrncpy(uname.machine, k_machine, sizeof(uname.machine));
    hostname_get(uname.nodename, sizeof(uname.nodename));

    ssize_t copied = user_copyout(buffer, &uname, sizeof(uname));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_uname, SYS_UNAME, 1);

static intptr_t sys_sethostname(user_addr_t name, size_t length)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(!cred_capable(thread->credentials, CAP_SYS_ADMIN)) {
        return -EPERM;
    }

    if(length > HOSTNAME_MAX) {
        return -EINVAL;
    }

    char hostname[HOSTNAME_MAX];
    ssize_t copied = user_copyin(hostname, name, length);

    if(copied < 0) {
        return copied;
    }

    return hostname_set(hostname, length);
}

define_syscall(sys_sethostname, SYS_SETHOSTNAME, 2);

static intptr_t sys_sysinfo(user_addr_t buffer)
{
    sysinfo_t info = {};
    info.uptime_ns = clock_source_now_ns();
    info.total_pages = pslab_numpages;
    info.free_pages = atomic_load(&pslab_numfree, ATOMIC_RELAXED);
    info.cpus_total = num_harts;

    for(unsigned long index = 0; index < num_harts; index += 1) {
        if(hart_online(index)) {
            info.cpus_online += 1;
        }
    }

    ssize_t copied = user_copyout(buffer, &info, sizeof(info));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_sysinfo, SYS_SYSINFO, 1);
