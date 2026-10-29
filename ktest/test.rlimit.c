#include <vex/capability.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/mman.h>
#include <vex/resource.h>
#include <vex/syscall.h>
#include <vex/wait.h>

#include "ktest.h"

#define PAGE_SIZE      4096
#define KIB            1024UL
#define MIB            (1024UL * KIB)
#define MAX_TEST_FDS   64
#define LOW_DESCRIPTOR 8

static const char* program_path;

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

// The probe runs in the program that exec made. It tells which of two places below the top of its stack are mapped:
// 100 KiB gives 1 when it is, and 300 KiB gives 2 when it is not
static int stack_child(void)
{
    char marker;
    long top = (long)(&marker);
    long inside = (top - 100 * KIB) & ~(long)(PAGE_SIZE - 1);
    long outside = (top - 300 * KIB) & ~(long)(PAGE_SIZE - 1);
    int code = 0;

    if(ktest_syscall3(__k_SYS_MADVISE, inside, PAGE_SIZE, __k_MADV_NORMAL) == 0) {
        code += 1;
    }

    if(ktest_syscall3(__k_SYS_MADVISE, outside, PAGE_SIZE, __k_MADV_NORMAL) == -__k_ENOMEM) {
        code += 2;
    }

    return code;
}

static void test_stack(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        const char* argv[] = { program_path, "stack-child", 0 };
        const char* envp[] = { 0 };

        // Less than the least stack the kernel gives out: the least comes
        if(set_limit(__k_RLIMIT_STACK, 16 * KIB, __k_RLIM_INFINITY) != 0) {
            ktest_syscall1(__k_SYS_EXIT, 100);
        }

        ktest_syscall3(__k_SYS_EXEC, (long)(program_path), (long)(argv), (long)(envp));
        ktest_syscall1(__k_SYS_EXIT, 99);
    }

    // 64 KiB is the least stack, so both places are out of it
    ktest_expect(wait_status(child), __k_W_EXITCODE(2, 0));

    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        const char* argv[] = { program_path, "stack-child", 0 };
        const char* envp[] = { 0 };

        if(set_limit(__k_RLIMIT_STACK, 2 * MIB, __k_RLIM_INFINITY) != 0) {
            ktest_syscall1(__k_SYS_EXIT, 100);
        }

        ktest_syscall3(__k_SYS_EXEC, (long)(program_path), (long)(argv), (long)(envp));
        ktest_syscall1(__k_SYS_EXIT, 99);
    }

    // Both places are in a stack of 2 MiB
    ktest_expect(wait_status(child), __k_W_EXITCODE(1, 0));
}

int main(int argc, char** argv)
{
    if(argc > 1 && ktest_string_equal(argv[1], "stack-child")) {
        return stack_child();
    }

    program_path = argv[0];

    test_basics();
    test_privilege();
    test_fork();
    test_nofile();
    test_stack();

    return ktest_result();
}
