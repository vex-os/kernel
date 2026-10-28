#include <vex/capability.h>
#include <vex/errno.h>
#include <vex/syscall.h>
#include <vex/utsname.h>

#include "ktest.h"

#define NODENAME_MAX (__k_UNAME_SIZE - 1)

static long set_hostname(const char* name, long length)
{
    return ktest_syscall2(__k_SYS_SETHOSTNAME, (long)(name), length);
}

static bool nodename_is(const char* expected)
{
    struct __k_utsname name = {};

    ktest_expect(ktest_syscall1(__k_SYS_UNAME, (long)(&name)), 0);

    return ktest_string_equal(name.nodename, expected);
}

static void test_sethostname(void)
{
    struct __k_utsname original = {};
    char longest[NODENAME_MAX + 1];

    ktest_expect(ktest_syscall1(__k_SYS_UNAME, (long)(&original)), 0);
    ktest_expect(original.nodename[0] != 0, 1);

    ktest_expect(set_hostname("kernel-test", 11), 0);
    ktest_expect(nodename_is("kernel-test"), 1);

    // The length decides where the name ends, not a terminator
    ktest_expect(set_hostname("abcdXXXX", 4), 0);
    ktest_expect(nodename_is("abcd"), 1);

    ktest_expect(set_hostname("", 0), 0);
    ktest_expect(nodename_is(""), 1);

    memset(longest, 'n', sizeof(longest));
    ktest_expect(set_hostname(longest, NODENAME_MAX), 0);
    ktest_expect(set_hostname(longest, NODENAME_MAX + 1), -__k_EINVAL);
    ktest_expect(set_hostname("abc", 3), 0);
    ktest_expect(set_hostname(nullptr, 4), -__k_EFAULT);
    ktest_expect(nodename_is("abc"), 1);

    // A process without CAP_SYS_ADMIN cannot rename the machine
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_caps caps;

        ktest_syscall1(__k_SYS_CAPGET, (long)(&caps));
        caps.effective &= ~__k_CAP_TO_MASK(__k_CAP_SYS_ADMIN);
        ktest_syscall1(__k_SYS_CAPSET, (long)(&caps));

        ktest_syscall1(__k_SYS_EXIT, -set_hostname("child", 5));
    }

    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0);
    ktest_expect(wstatus >> 8, __k_EPERM & 0xFF);
    ktest_expect(nodename_is("abc"), 1);

    ktest_expect(set_hostname(original.nodename, ktest_strlen(original.nodename)), 0);
    ktest_expect(nodename_is(original.nodename), 1);
}

int main(int argc, char** argv)
{
    struct __k_utsname name = {};

    long retval = ktest_syscall1(__k_SYS_UNAME, (long)(&name));

    if(retval < 0) {
        ktest_fail("uname failed: %ld", retval);
    }

    ktest_printf("%s %s %s %s\n", name.sysname, name.release, name.version, name.machine);
    ktest_assert(ktest_string_equal(name.sysname, "Vex"));

    test_sethostname();

    return ktest_result();
}
