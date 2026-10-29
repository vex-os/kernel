#include <vex/capability.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/resource.h>
#include <vex/syscall.h>
#include <vex/wait.h>

#include "ktest.h"

#define MAX_TEST_FDS   64
#define LOW_DESCRIPTOR 8

static long get_limit(long resource, struct __k_rlimit* restrict limit)
{
    return ktest_syscall2(__k_SYS_GETRLIMIT, resource, (long)(limit));
}

static long set_limit(long resource, __k_uint64_t cur, __k_uint64_t max)
{
    struct __k_rlimit limit = {};
    limit.rlim_cur = cur;
    limit.rlim_max = max;

    return ktest_syscall2(__k_SYS_SETRLIMIT, resource, (long)(&limit));
}

static bool limit_is(long resource, __k_uint64_t cur, __k_uint64_t max)
{
    struct __k_rlimit limit = {};

    ktest_expect(get_limit(resource, &limit), 0);

    return limit.rlim_cur == cur && limit.rlim_max == max;
}

static long wait_status(long child)
{
    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0);

    return wstatus;
}

static void test_basics(void)
{
    struct __k_rlimit limit = {};

    ktest_expect(get_limit(__k_RLIMIT_NLIMITS, &limit), -__k_EINVAL);
    ktest_expect(get_limit(__k_RLIMIT_CPU, nullptr), -__k_EFAULT);
    ktest_expect(limit_is(__k_RLIMIT_CPU, __k_RLIM_INFINITY, __k_RLIM_INFINITY), 1);

    ktest_expect(get_limit(__k_RLIMIT_NOFILE, &limit), 0);
    ktest_expect(limit.rlim_cur > LOW_DESCRIPTOR, 1);
    ktest_expect(limit.rlim_cur == limit.rlim_max, 1);

    ktest_expect(set_limit(__k_RLIMIT_NLIMITS, 1, 1), -__k_EINVAL);
    ktest_expect(set_limit(__k_RLIMIT_CPU, 10, 5), -__k_EINVAL);
    ktest_expect(ktest_syscall2(__k_SYS_SETRLIMIT, __k_RLIMIT_CPU, 0), -__k_EFAULT);

    ktest_expect(set_limit(__k_RLIMIT_CPU, 5, 10), 0);
    ktest_expect(limit_is(__k_RLIMIT_CPU, 5, 10), 1);

    // A limit never takes the others along
    ktest_expect(limit_is(__k_RLIMIT_FSIZE, __k_RLIM_INFINITY, __k_RLIM_INFINITY), 1);

    // The descriptor table can not get bigger than the kernel makes it, whatever the process is
    ktest_expect(set_limit(__k_RLIMIT_NOFILE, 1, __k_RLIM_INFINITY), -__k_EPERM);
}

static void test_privilege(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_caps caps;

        ktest_syscall1(__k_SYS_CAPGET, (long)(&caps));
        caps.effective &= ~__k_CAP_TO_MASK(__k_CAP_SYS_RESOURCE);
        ktest_syscall1(__k_SYS_CAPSET, (long)(&caps));

        int code = 0;

        // The soft limit moves up to the hard one, and the hard one only goes down
        if(set_limit(__k_RLIMIT_CPU, 10, 10) == 0) {
            code += 1;
        }

        if(set_limit(__k_RLIMIT_CPU, 10, 11) == -__k_EPERM) {
            code += 2;
        }

        if(set_limit(__k_RLIMIT_CPU, 3, 4) == 0) {
            code += 4;
        }

        if(set_limit(__k_RLIMIT_CPU, 3, 10) == -__k_EPERM) {
            code += 8;
        }

        ktest_syscall1(__k_SYS_EXIT, code);
    }

    ktest_expect(wait_status(child), __k_W_EXITCODE(15, 0));
}

static void test_fork(void)
{
    ktest_expect(set_limit(__k_RLIMIT_DATA, 7, 9), 0);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        int code = 0;

        if(limit_is(__k_RLIMIT_DATA, 7, 9)) {
            code += 1;
        }

        if(set_limit(__k_RLIMIT_DATA, 1, 2) == 0) {
            code += 2;
        }

        if(limit_is(__k_RLIMIT_DATA, 1, 2)) {
            code += 4;
        }

        ktest_syscall1(__k_SYS_EXIT, code);
    }

    ktest_expect(wait_status(child), __k_W_EXITCODE(7, 0));

    // The child changed its own limits, not the shared ones
    ktest_expect(limit_is(__k_RLIMIT_DATA, 7, 9), 1);
}

static void test_nofile(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        int code = 0;
        long first = -1;
        long last = -1;
        long fd = 0;

        if(set_limit(__k_RLIMIT_NOFILE, LOW_DESCRIPTOR, LOW_DESCRIPTOR) != 0) {
            ktest_syscall1(__k_SYS_EXIT, 100);
        }

        // Opening goes on until the table is full, and no descriptor is at or above the limit
        for(int i = 0; i < MAX_TEST_FDS && fd >= 0; ++i) {
            fd = ktest_open(".", __k_O_RDONLY, 0);

            if(fd >= 0 && first < 0) {
                first = fd;
            }

            if(fd >= 0) {
                last = fd;
            }
        }

        if(fd == -__k_EMFILE && last == LOW_DESCRIPTOR - 1) {
            code += 1;
        }

        if(ktest_syscall2(__k_SYS_DUP2, first, LOW_DESCRIPTOR) == -__k_EBADF) {
            code += 2;
        }

        if(ktest_syscall2(__k_SYS_DUP2, first, LOW_DESCRIPTOR - 1) == LOW_DESCRIPTOR - 1) {
            code += 4;
        }

        if(ktest_syscall3(__k_SYS_FCNTL, first, __k_F_DUPFD, LOW_DESCRIPTOR) == -__k_EINVAL) {
            code += 8;
        }

        // A limit under the descriptors in use turns away new ones and keeps the old ones
        if(set_limit(__k_RLIMIT_NOFILE, 2, LOW_DESCRIPTOR) == 0 && ktest_open(".", __k_O_RDONLY, 0) == -__k_EMFILE) {
            code += 16;
        }

        if(ktest_syscall3(__k_SYS_FCNTL, last, __k_F_GETFD, 0) >= 0) {
            code += 32;
        }

        ktest_syscall1(__k_SYS_EXIT, code);
    }

    ktest_expect(wait_status(child), __k_W_EXITCODE(63, 0));
}

int main(int argc, char** argv)
{
    test_basics();
    test_privilege();
    test_fork();
    test_nofile();

    return ktest_result();
}
