#include <vex/capability.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/mman.h>
#include <vex/resource.h>
#include <vex/signal.h>
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

    ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, 0);

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

        if(set_limit(__k_RLIMIT_NOFILE, LOW_DESCRIPTOR, LOW_DESCRIPTOR)) {
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

        if(ktest_syscall3(__k_SYS_DUP, first, LOW_DESCRIPTOR, 0) == -__k_EBADF) {
            code += 2;
        }

        if(ktest_syscall3(__k_SYS_DUP, first, LOW_DESCRIPTOR - 1, 0) == LOW_DESCRIPTOR - 1) {
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

static long parse_kib(const char* text)
{
    long value = 0;

    while(*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text += 1;
    }

    return value;
}

// The probe runs in the program that exec made. The page that depth KiB below the top of its stack is in
// is not mapped at first, and a write to it grows the stack, or kills the program past the limit
static int stack_child(long depth)
{
    char marker;
    long top = (long)(&marker);
    long page = (top - depth * KIB) & ~(long)(PAGE_SIZE - 1);

    if(ktest_syscall3(__k_SYS_MADVISE, page, PAGE_SIZE, __k_MADV_NORMAL) != -__k_ENOMEM) {
        return 1;
    }

    *(volatile char*)(page) = 1;

    if(ktest_syscall3(__k_SYS_MADVISE, page, PAGE_SIZE, __k_MADV_NORMAL)) {
        return 2;
    }

    return 0;
}

static volatile bool stop_recursion; // Never set; the compiler can't tell, and lets the recursion be

// Each call keeps a page of its own alive, so the compiler can neither drop the frame nor make it a loop
static long recurse(long depth)
{
    if(stop_recursion) {
        return 0;
    }

    volatile char frame[PAGE_SIZE];
    frame[0] = (char)(depth);

    return recurse(depth + 1) + frame[0];
}

static long run_stack_child(long limit, const char* depth)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        const char* argv[] = { program_path, "stack-child", depth, 0 };

        if(depth == nullptr) {
            argv[1] = "stack-overflow";
        }
        const char* envp[] = { 0 };

        if(set_limit(__k_RLIMIT_STACK, limit, __k_RLIM_INFINITY)) {
            ktest_syscall1(__k_SYS_EXIT, 100);
        }

        ktest_syscall3(__k_SYS_EXEC, (long)(program_path), (long)(argv), (long)(envp));
        ktest_syscall1(__k_SYS_EXIT, 99);
    }

    return wait_status(child);
}

static void test_stack(void)
{
    // Less than the least stack the kernel gives out, which is 64 KiB
    ktest_expect(run_stack_child(16 * KIB, "100"), __k_SIGSEGV);

    // The stack grows to where it is touched, also past a gap, up to the limit
    ktest_expect(run_stack_child(2 * MIB, "300"), __k_W_EXITCODE(0, 0));
    ktest_expect(run_stack_child(2 * MIB, "1900"), __k_W_EXITCODE(0, 0));
    ktest_expect(run_stack_child(2 * MIB, "2100"), __k_SIGSEGV);

    // Endless recursion ends in SIGSEGV at the limit
    ktest_expect(run_stack_child(512 * KIB, nullptr), __k_SIGSEGV);
}

int main(int argc, char** argv)
{
    if(argc > 1 && ktest_string_equal(argv[1], "stack-overflow")) {
        return (int)(recurse(0));
    }

    if(argc > 2 && ktest_string_equal(argv[1], "stack-child")) {
        return stack_child(parse_kib(argv[2]));
    }

    program_path = argv[0];

    test_basics();
    test_privilege();
    test_fork();
    test_nofile();
    test_stack();

    return ktest_result();
}
