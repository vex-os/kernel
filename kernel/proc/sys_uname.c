#include <vex/capability.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/hostname.h>
#include <vex/process.h>
#include <vex/strings.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/utsname.h>
#include <vex/version.h>

static intptr_t sys_uname(const uintptr_t* restrict args)
{
    utsname_t uname;
    kstrncpy(uname.sysname, k_sysname, sizeof(uname.sysname));
    kstrncpy(uname.release, k_release, sizeof(uname.release));
    kstrncpy(uname.version, k_version, sizeof(uname.version));
    kstrncpy(uname.machine, k_machine, sizeof(uname.machine));
    hostname_get(uname.nodename, sizeof(uname.nodename));

    ssize_t copied = user_copyout(args[0], &uname, sizeof(uname));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_uname, SYS_UNAME);

static intptr_t sys_sethostname(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(!cred_capable(thread->credentials, CAP_SYS_ADMIN)) {
        return -EPERM;
    }

    size_t length = args[1];

    if(length > HOSTNAME_MAX) {
        return -EINVAL;
    }

    char name[HOSTNAME_MAX];
    ssize_t copied = user_copyin(name, args[0], length);

    if(copied < 0) {
        return copied;
    }

    return hostname_set(name, length);
}

define_syscall(sys_sethostname, SYS_SETHOSTNAME);
