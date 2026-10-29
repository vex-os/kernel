#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/limits.h>
#include <vex/mman.h>
#include <vex/mount.h>
#include <vex/stat.h>
#include <vex/statvfs.h>
#include <vex/syscall.h>

#include "ktest.h"

#define PAGE_SIZE      4096
#define SMALL_BUFFER   32
#define LIMITED_PAGES  4
#define LIMITED_NODES  3
#define RECORD_BUFFER  1024
#define MOUNT_RW       0
#define SHARED_RW      (__k_PROT_READ | __k_PROT_WRITE)
#define FILE_MODE      0644
#define DIRECTORY_MODE 0755

static char mount_point[__k_PATH_MAX];

static long make_directory(const char* path)
{
    return ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)(path), DIRECTORY_MODE);
}

static long remove_directory(const char* path)
{
    return ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(path), __k_AT_REMOVEDIR);
}

static long remove_file(const char* path)
{
    return ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(path), 0);
}

static long mount_tmpfs(const char* target, const char* params)
{
    return ktest_syscall6(__k_SYS_MOUNT, 0, (long)(target), (long)("tmpfs"), MOUNT_RW, (long)(params), 0);
}

static long unmount(const char* target)
{
    return ktest_syscall1(__k_SYS_UMOUNT, (long)(target));
}

static long truncate_to(long fd, long size)
{
    return ktest_syscall2(__k_SYS_FTRUNCATE, fd, size);
}

static long read_at(long fd, void* buffer, size_t size, long offset)
{
    return ktest_syscall4(__k_SYS_PREAD, fd, (long)(buffer), (long)(size), offset);
}

static long write_at(long fd, const void* buffer, size_t size, long offset)
{
    return ktest_syscall4(__k_SYS_PWRITE, fd, (long)(buffer), (long)(size), offset);
}

static void path_in(char* buffer, const char* directory, const char* name)
{
    ktest_snprintf(buffer, __k_PATH_MAX, "%s/%s", directory, name);
}

static long create_file(const char* directory, const char* name)
{
    char path[__k_PATH_MAX];

    path_in(path, directory, name);

    return ktest_open(path, __k_O_RDWR | __k_O_CREAT | __k_O_EXCL, FILE_MODE);
}

static void get_statvfs(const char* path, struct __k_statvfs* restrict info)
{
    long fd = ktest_open(path, __k_O_RDONLY, 0);

    ktest_assert(fd >= 0);
    ktest_expect(ktest_syscall2(__k_SYS_STATVFS, fd, (long)(info)), 0);
    ktest_close(fd);
}

static long free_blocks(const char* path)
{
    struct __k_statvfs info = {};

    get_statvfs(path, &info);

    return (long)(info.f_bfree);
}

static void test_contents(void)
{
    char path[__k_PATH_MAX];
    struct __k_stat info;
    char buffer[16] = {};
    long fd = create_file(mount_point, "a");

    ktest_assert(fd >= 0);
    ktest_expect(ktest_write(fd, "hello", 5), 5);
    ktest_expect(ktest_syscall2(__k_SYS_FSTAT, fd, (long)(&info)), 0);
    ktest_expect(info.st_size, 5);
    ktest_expect(info.st_nlink, 1);
    ktest_expect(info.st_mode, __k_S_IFREG | FILE_MODE);

    ktest_expect(read_at(fd, buffer, sizeof(buffer), 1), 4);
    ktest_assert(ktest_bytes_equal(buffer, "ello", 4));

    // A hole reads as zeroes, and a read past the end gives nothing
    ktest_expect(write_at(fd, "z", 1, 3 * PAGE_SIZE), 1);
    ktest_expect(ktest_syscall2(__k_SYS_FSTAT, fd, (long)(&info)), 0);
    ktest_expect(info.st_size, 3 * PAGE_SIZE + 1);
    ktest_expect(read_at(fd, buffer, 4, PAGE_SIZE), 4);
    ktest_assert(ktest_bytes_equal(buffer, "\0\0\0\0", 4));
    ktest_expect(read_at(fd, buffer, 4, 3 * PAGE_SIZE + 1), 0);

    // Cut data does not come back when the file grows again
    ktest_expect(truncate_to(fd, 3), 0);
    ktest_expect(truncate_to(fd, 2 * PAGE_SIZE), 0);
    ktest_expect(read_at(fd, buffer, 5, 0), 5);
    ktest_assert(ktest_bytes_equal(buffer, "hel\0\0", 5));

    ktest_close(fd);
    path_in(path, mount_point, "a");
    ktest_expect(remove_file(path), 0);
    ktest_expect(remove_file(path), -__k_ENOENT);
}

static void test_open_unlinked(void)
{
    char path[__k_PATH_MAX];
    struct __k_stat info;
    char buffer[8] = {};
    long fd = create_file(mount_point, "gone");

    ktest_assert(fd >= 0);
    ktest_expect(ktest_write(fd, "still", 5), 5);

    path_in(path, mount_point, "gone");
    ktest_expect(remove_file(path), 0);
    ktest_expect(ktest_open(path, __k_O_RDONLY, 0), -__k_ENOENT);

    // The file lives on for whoever holds it, and the mount is busy meanwhile
    ktest_expect(ktest_syscall2(__k_SYS_FSTAT, fd, (long)(&info)), 0);
    ktest_expect(info.st_nlink, 0);
    ktest_expect(read_at(fd, buffer, sizeof(buffer), 0), 5);
    ktest_assert(ktest_bytes_equal(buffer, "still", 5));
    ktest_expect(unmount(mount_point), -__k_EBUSY);

    ktest_close(fd);
}

static void test_mapping(void)
{
    char buffer[8] = {};
    long fd = create_file(mount_point, "map");

    ktest_assert(fd >= 0);
    ktest_expect(truncate_to(fd, PAGE_SIZE), 0);

    long first = ktest_syscall6(__k_SYS_MMAP, 0, PAGE_SIZE, SHARED_RW, __k_MAP_SHARED, fd, 0);
    long second = ktest_syscall6(__k_SYS_MMAP, 0, PAGE_SIZE, SHARED_RW, __k_MAP_SHARED, fd, 0);

    ktest_assert(first > 0 && second > 0);

    // Two mappings, and read and write, see the same page
    *(volatile char*)(first) = 'M';
    ktest_expect(*(volatile char*)(second), 'M');
    ktest_expect(read_at(fd, buffer, 1, 0), 1);
    ktest_expect(buffer[0], 'M');

    ktest_expect(write_at(fd, "W", 1, 1), 1);
    ktest_expect(*(volatile char*)(second + 1), 'W');

    ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, first, PAGE_SIZE), 0);
    ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, second, PAGE_SIZE), 0);
    ktest_close(fd);
}

static void test_directories(void)
{
    char directory[__k_PATH_MAX];
    char path[__k_PATH_MAX];
    struct __k_stat info;

    path_in(directory, mount_point, "d");
    ktest_expect(make_directory(directory), 0);
    ktest_expect(make_directory(directory), -__k_EEXIST);

    ktest_expect(ktest_syscall4(__k_SYS_FSTATAT, __k_AT_FDCWD, (long)(mount_point), (long)(&info), 0), 0);
    ktest_expect(info.st_nlink, 3);

    long fd = create_file(directory, "f");

    ktest_assert(fd >= 0);
    ktest_close(fd);

    ktest_expect(remove_directory(directory), -__k_ENOTEMPTY);

    path_in(path, directory, "f");
    ktest_expect(remove_file(path), 0);
    ktest_expect(remove_directory(directory), 0);

    ktest_expect(ktest_syscall4(__k_SYS_FSTATAT, __k_AT_FDCWD, (long)(mount_point), (long)(&info), 0), 0);
    ktest_expect(info.st_nlink, 2);
}

// Reads the directory a record at a time, and removes one name that was not read yet half way
static void test_iteration(void)
{
    char directory[__k_PATH_MAX];
    char path[__k_PATH_MAX];
    char name[4];
    alignas(8) char records[RECORD_BUFFER];
    long count = 0;
    bool seen[5] = {};

    path_in(directory, mount_point, "list");
    ktest_expect(make_directory(directory), 0);

    for(int i = 0; i < 5; ++i) {
        ktest_snprintf(name, sizeof(name), "f%d", i);
        ktest_close(create_file(directory, name));
    }

    long fd = ktest_open(directory, __k_O_RDONLY, 0);

    ktest_assert(fd >= 0);

    for(;;) {
        long filled = ktest_syscall3(__k_SYS_GETDENTS, fd, (long)(records), SMALL_BUFFER);

        ktest_assert(filled >= 0);

        if(filled == 0) {
            break;
        }

        const struct __k_dirent* entry = (const struct __k_dirent*)(records);
        const char* entry_name = (const char*)(entry->d_name);

        if(entry_name[0] == 'f') {
            seen[entry_name[1] - '0'] = true;
            count += 1;

            // A cookie goes on after the names around it are gone
            if(count == 2) {
                path_in(path, directory, "f2");
                ktest_expect(remove_file(path), 0);
            }
        }
    }

    ktest_close(fd);

    ktest_expect(count, 4);
    ktest_assert(seen[0] && seen[1] && !seen[2] && seen[3] && seen[4]);

    for(int i = 0; i < 5; ++i) {
        ktest_snprintf(name, sizeof(name), "f%d", i);
        path_in(path, directory, name);
        remove_file(path);
    }

    ktest_expect(remove_directory(directory), 0);
}

static void test_accounting(void)
{
    long before = free_blocks(mount_point);
    long fd = create_file(mount_point, "pages");

    ktest_assert(fd >= 0);
    ktest_expect(truncate_to(fd, 2 * PAGE_SIZE + 1), 0);
    ktest_expect(free_blocks(mount_point), before - 3);

    ktest_expect(truncate_to(fd, PAGE_SIZE), 0);
    ktest_expect(free_blocks(mount_point), before - 1);

    // The pages go with the last reference, also of a file with no name
    char path[__k_PATH_MAX];

    path_in(path, mount_point, "pages");
    ktest_expect(remove_file(path), 0);
    ktest_expect(free_blocks(mount_point), before - 1);
    ktest_close(fd);
    ktest_expect(free_blocks(mount_point), before);
}

static void test_limits(const char* work_directory)
{
    char limited[__k_PATH_MAX];
    char buffer[PAGE_SIZE] = {};
    struct __k_statvfs info;

    path_in(limited, work_directory, "limited");
    ktest_expect(make_directory(limited), 0);
    ktest_expect(mount_tmpfs(limited, "size=16k nodes=3 mode=0700"), 0);

    get_statvfs(limited, &info);
    ktest_expect(info.f_blocks, LIMITED_PAGES);
    ktest_expect(info.f_files, LIMITED_NODES);

    long first = create_file(limited, "one");
    long second = create_file(limited, "two");

    ktest_assert(first >= 0 && second >= 0);
    ktest_expect(create_file(limited, "three"), -__k_ENOSPC);

    for(long page = 0; page < LIMITED_PAGES; ++page) {
        ktest_expect(write_at(first, buffer, sizeof(buffer), page * PAGE_SIZE), PAGE_SIZE);
    }

    // Both the write and the resize fail whole, and the file stays as it was
    ktest_expect(write_at(second, buffer, 1, 0), -__k_ENOSPC);
    ktest_expect(write_at(first, buffer, 1, LIMITED_PAGES * PAGE_SIZE), -__k_ENOSPC);
    ktest_expect(truncate_to(first, (LIMITED_PAGES + 1) * PAGE_SIZE), -__k_ENOSPC);
    ktest_expect(free_blocks(limited), 0);

    ktest_expect(truncate_to(first, PAGE_SIZE), 0);
    ktest_expect(write_at(second, buffer, 1, 0), 1);
    ktest_expect(free_blocks(limited), LIMITED_PAGES - 2);

    ktest_close(first);
    ktest_close(second);

    ktest_expect(unmount(limited), 0);
    ktest_expect(mount_tmpfs(limited, "size=nonsense"), -__k_EINVAL);
    ktest_expect(remove_directory(limited), 0);
}

int main(void)
{
    const char* work_directory = ktest_enter_work_directory("tmpfs");
    char path[__k_PATH_MAX];

    path_in(mount_point, work_directory, "tmp");
    ktest_expect(make_directory(mount_point), 0);
    ktest_expect(mount_tmpfs(mount_point, nullptr), 0);

    test_contents();
    test_open_unlinked();
    test_mapping();
    test_directories();
    test_iteration();
    test_accounting();

    path_in(path, mount_point, "map");
    ktest_expect(remove_file(path), 0);
    ktest_expect(unmount(mount_point), 0);

    test_limits(work_directory);

    return ktest_result();
}
