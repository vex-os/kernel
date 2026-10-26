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

    return ktest_result();
}
