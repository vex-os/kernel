#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/limits.h>
#include <vex/syscall.h>

#include "ktest.h"

#define UNPRIVILEGED_ID 1000

static long make_directory(const char* path)
{
    return ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)(path), 0755);
}

static long change_root(const char* path)
{
    return ktest_syscall1(__k_SYS_CHROOT, (long)(path));
}

static long change_directory(const char* path)
{
    return ktest_syscall1(__k_SYS_CHDIR, (long)(path));
}

static void make_file(const char* path, const char* text)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_WRONLY, 0644);

    ktest_assert(fd >= 0);
    ktest_write(fd, text, ktest_strlen(text));
    ktest_close(fd);
}

static void expect_cwd(const char* expected)
{
    char buffer[__k_PATH_MAX];

    ktest_expect(ktest_syscall2(__k_SYS_GETCWD, (long)(buffer), sizeof(buffer)), (long)(ktest_strlen(expected) + 1));
    ktest_expect(ktest_string_equal(buffer, expected), true);
}

static void run_in_child(void (*body)(void))
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        body();
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    int wstatus = -1;

    ktest_expect(ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0), child);
    ktest_expect(wstatus, 0);
}

static void inside_the_jail(void)
{
    ktest_expect(change_root("jail"), 0);

    // The tree of the jail starts at the slash, and the dots cannot climb out of it
    ktest_expect(ktest_file_has_prefix("/inner/data", "jail"), true);
    ktest_expect(ktest_file_has_prefix("/../inner/data", "jail"), true);
    ktest_expect(ktest_file_has_prefix("/../../../inner/data", "jail"), true);
    ktest_expect(ktest_open("/outside", __k_O_RDONLY, 0), -__k_ENOENT);
    ktest_expect(ktest_open("/../outside", __k_O_RDONLY, 0), -__k_ENOENT);

    // An absolute symlink starts at the new root too
    ktest_expect(ktest_file_has_prefix("/abs", "jail"), true);

    // A directory inside is named from the new root, and its dots stop there
    ktest_expect(change_directory("/inner"), 0);
    expect_cwd("/inner");
    ktest_expect(ktest_file_has_prefix("../../../inner/data", "jail"), true);
    ktest_expect(change_directory("../.."), 0);
    expect_cwd("/");

    // A jail can have a smaller jail inside
    ktest_expect(change_root("/inner"), 0);
    ktest_expect(ktest_file_has_prefix("/data", "jail"), true);
    ktest_expect(ktest_open("/abs", __k_O_RDONLY, 0), -__k_ENOENT);

    // A file is not a root
    ktest_expect(change_root("/data"), -__k_ENOTDIR);
    ktest_expect(change_root("/missing"), -__k_ENOENT);
}

static void without_privilege(void)
{
    ktest_expect(ktest_syscall3(__k_SYS_SETRESUID, UNPRIVILEGED_ID, UNPRIVILEGED_ID, UNPRIVILEGED_ID), 0);
    ktest_expect(change_root("jail"), -__k_EPERM);
    ktest_expect(ktest_file_has_prefix("outside", "out"), true);
}

// A fork inherits the root
static void inherited(void)
{
    ktest_expect(change_root("jail"), 0);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        ktest_expect(ktest_file_has_prefix("/inner/data", "jail"), true);
        ktest_expect(ktest_open("/outside", __k_O_RDONLY, 0), -__k_ENOENT);
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    int wstatus = -1;

    ktest_expect(ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0), child);
    ktest_expect(wstatus, 0);
}

int main(void)
{
    ktest_enter_work_directory("chroot");

    ktest_expect(make_directory("jail"), 0);
    ktest_expect(make_directory("jail/inner"), 0);
    make_file("jail/inner/data", "jail\n");
    make_file("outside", "out\n");
    ktest_expect(ktest_syscall3(__k_SYS_SYMLINKAT, (long)("/inner/data"), __k_AT_FDCWD, (long)("jail/abs")), 0);

    run_in_child(&inside_the_jail);
    run_in_child(&without_privilege);
    run_in_child(&inherited);

    // None of that touched this process
    ktest_expect(ktest_file_has_prefix("outside", "out"), true);

    return ktest_result();
}
