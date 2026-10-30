#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/syscall.h>

#include "ktest.h"

#define FAT_DEVICE "/dev/ada0n1p3"

static long make_directory(const char* path)
{
    return ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)(path), 0755);
}

static long mount_filesystem(const char* source, const char* target, const char* type, long flags, const char* params)
{
    return ktest_syscall6(__k_SYS_MOUNT, (long)(source), (long)(target), (long)(type), flags, (long)(params), 0);
}

static void expect_read_only(const char* mount_point)
{
    char directory[__k_PATH_MAX];

    ktest_snprintf(directory, sizeof(directory), "%s/new", mount_point);
    ktest_expect(make_directory(directory), -__k_EROFS);
}

static long unmount(const char* target)
{
    return ktest_syscall1(__k_SYS_UMOUNT, (long)(target));
}

static long make_file(const char* path)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_WRONLY, 0644);

    if(fd < 0) {
        return fd;
    }

    ktest_close(fd);
    return 0;
}

// The mount point is a path like any other: from the working directory, in any spelling that leads to it
static void test_paths(void)
{
    ktest_expect(make_directory("memory"), 0);
    ktest_expect(mount_filesystem(nullptr, "memory", "tmpfs", 0, nullptr), 0);
    ktest_expect(make_file("memory/inside"), 0);

    ktest_expect(unmount("./memory/"), 0);
    ktest_expect(ktest_open("memory/inside", __k_O_RDONLY, 0), -__k_ENOENT);

    // A directory with nothing mounted on it, and a file, are no mount points
    ktest_expect(unmount("memory"), -__k_EINVAL);
    ktest_expect(make_file("plain"), 0);
    ktest_expect(mount_filesystem(nullptr, "plain", "tmpfs", 0, nullptr), -__k_ENOTDIR);
    ktest_expect(mount_filesystem(nullptr, "missing", "tmpfs", 0, nullptr), -__k_ENOENT);
}

// A path from the root of a process in chroot starts at its new root
static void mount_in_jail(void)
{
    ktest_expect(ktest_syscall1(__k_SYS_CHROOT, (long)("jail")), 0);
    ktest_expect(ktest_syscall1(__k_SYS_CHDIR, (long)("/")), 0);
    ktest_expect(mount_filesystem(nullptr, "/inner", "tmpfs", 0, nullptr), 0);
    ktest_expect(make_file("/inner/mark"), 0);
    ktest_expect(unmount("/inner"), 0);
}

static void test_chroot(void)
{
    ktest_expect(make_directory("jail"), 0);
    ktest_expect(make_directory("jail/inner"), 0);
    ktest_expect(make_directory("inner"), 0);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        mount_in_jail();
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    ktest_assert(child > 0);

    int wstatus = -1;
    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, 0), child);
    ktest_expect(wstatus, 0);

    // The mount went on jail/inner, and nothing touched the inner of the work directory
    ktest_expect(unmount("inner"), -__k_EINVAL);
}

int main(void)
{
    const char* work_directory = ktest_enter_work_directory("mount");
    char mount_point[__k_PATH_MAX];
    char directory[__k_PATH_MAX];

    ktest_snprintf(mount_point, sizeof(mount_point), "%s/fat", work_directory);
    ktest_snprintf(directory, sizeof(directory), "%s/new", mount_point);
    ktest_expect(make_directory(mount_point), 0);

    // A device is enough to find the type, and the flag asks for the read-only mount
    ktest_expect(mount_filesystem(FAT_DEVICE, mount_point, nullptr, __k_MNT_RDONLY, nullptr), 0);
    expect_read_only(mount_point);
    ktest_expect(ktest_syscall1(__k_SYS_UMOUNT, (long)(mount_point)), 0);

    // The parameter does the same
    ktest_expect(mount_filesystem(FAT_DEVICE, mount_point, "fat", 0, "ro"), 0);
    expect_read_only(mount_point);
    ktest_expect(ktest_syscall1(__k_SYS_UMOUNT, (long)(mount_point)), 0);

    ktest_expect(mount_filesystem(FAT_DEVICE, mount_point, "fat", 0, nullptr), 0);
    ktest_expect(make_directory(directory), 0);
    ktest_expect(ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(directory), __k_AT_REMOVEDIR), 0);
    ktest_expect(ktest_syscall1(__k_SYS_UMOUNT, (long)(mount_point)), 0);

    // No flag but the known ones, and no mount without a device or a type
    ktest_expect(mount_filesystem(FAT_DEVICE, mount_point, "fat", 0x8000, nullptr), -__k_EINVAL);
    ktest_expect(mount_filesystem(nullptr, mount_point, nullptr, 0, nullptr), -__k_EINVAL);

    test_paths();
    test_chroot();

    return ktest_result();
}
