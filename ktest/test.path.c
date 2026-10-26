#include <vex/capability.h>
#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/limits.h>
#include <vex/syscall.h>

#include "ktest.h"

#define SMALL_BUFFER_SIZE 4
#define UNPRIVILEGED_ID   1000
#define KEEP_ID           (-1)

static long make_directory(const char* path)
{
    return ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)(path), 0755);
}

static long get_cwd(char* buffer, size_t size)
{
    return ktest_syscall2(__k_SYS_GETCWD, (long)(buffer), (long)(size));
}

static long check_access(const char* path, long mode, long flags)
{
    return ktest_syscall4(__k_SYS_FACCESSAT, __k_AT_FDCWD, (long)(path), mode, flags);
}

static void expect_cwd(const char* expected)
{
    char buffer[__k_PATH_MAX];

    ktest_expect(get_cwd(buffer, sizeof(buffer)), (long)(ktest_strlen(expected) + 1));
    ktest_expect(ktest_string_equal(buffer, expected), true);
}

static void test_getcwd(const char* work_directory)
{
    char nested[__k_PATH_MAX];
    char buffer[__k_PATH_MAX];

    ktest_snprintf(nested, sizeof(nested), "%s/outer/inner", work_directory);

    expect_cwd(work_directory);

    ktest_expect(make_directory("outer"), 0);
    ktest_expect(make_directory("outer/inner"), 0);
    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)("outer/inner")), 0);
    expect_cwd(nested);

    // The dots do not show in the path
    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)("../..")), 0);
    expect_cwd(work_directory);

    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)("/")), 0);
    expect_cwd("/");

    // The cwd of a mounted filesystem has the name of its mountpoint
    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)("/dev")), 0);
    expect_cwd("/dev");

    // The terminating zero must fit too
    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)(nested)), 0);
    ktest_expect(get_cwd(buffer, ktest_strlen(nested)), -__k_ERANGE);
    ktest_expect(get_cwd(buffer, ktest_strlen(nested) + 1), (long)(ktest_strlen(nested) + 1));
    ktest_expect(get_cwd(buffer, SMALL_BUFFER_SIZE), -__k_ERANGE);

    // A directory with no name left is not under the root
    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)("/")), 0);
    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)(nested)), 0);
    ktest_expect(ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(nested), __k_AT_REMOVEDIR), 0);
    ktest_expect(get_cwd(buffer, sizeof(buffer)), -__k_ENOENT);

    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)(work_directory)), 0);
}

// The process is root with the capabilities, until its real uid changes
static void test_faccessat(void)
{
    long fd = ktest_open("secret", __k_O_CREAT | __k_O_WRONLY, 0600);

    ktest_assert(fd >= 0);
    ktest_close(fd);

    ktest_expect(check_access("secret", __k_F_OK, 0), 0);
    ktest_expect(check_access("secret", __k_R_OK | __k_W_OK, 0), 0);
    ktest_expect(check_access("missing", __k_F_OK, 0), -__k_ENOENT);
    ktest_expect(check_access("secret", 0x10, 0), -__k_EINVAL);
    ktest_expect(check_access("secret", __k_F_OK, 0x8000), -__k_EINVAL);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        // Real uid of a user, effective uid of root: access goes by the real one
        ktest_expect(ktest_syscall3(__k_SYS_SETRESUID, UNPRIVILEGED_ID, 0, 0), 0);
        ktest_expect(check_access("secret", __k_R_OK, 0), -__k_EACCES);
        ktest_expect(check_access("secret", __k_R_OK, __k_AT_EACCESS), 0);
        ktest_expect(check_access("secret", __k_F_OK, 0), 0);

        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    int wstatus = -1;

    ktest_expect(ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0), child);
    ktest_expect(wstatus, 0);

    ktest_expect(ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)("secret"), 0), 0);
}

int main(void)
{
    const char* work_directory = ktest_enter_work_directory("path");

    test_getcwd(work_directory);
    test_faccessat();

    return ktest_result();
}
