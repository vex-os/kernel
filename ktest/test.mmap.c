#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/mman.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/wait.h>

#include "ktest.h"

#define PAGE_SIZE   4096
#define FILE_PAGES  3
#define FILE_SIZE   (FILE_PAGES * PAGE_SIZE)
#define MANY_PAGES  64
#define HUGE_LENGTH (1L << 62)

#define RW         (__k_PROT_READ | __k_PROT_WRITE)
#define PRIVATE_AN (__k_MAP_PRIVATE | __k_MAP_ANONYMOUS)

static long map(long address, long length, long prot, long flags, long fd, long offset)
{
    return ktest_syscall6(__k_SYS_MMAP, address, length, prot, flags, fd, offset);
}

static long unmap(long address, long length)
{
    return ktest_syscall2(__k_SYS_MUNMAP, address, length);
}

static long protect(long address, long length, long prot)
{
    return ktest_syscall3(__k_SYS_MPROTECT, address, length, prot);
}

static long sync_range(long address, long length, long flags)
{
    return ktest_syscall3(__k_SYS_MSYNC, address, length, flags);
}

static long seek_to(long fd, long offset)
{
    return ktest_syscall3(__k_SYS_SEEK, fd, offset, __k_SEEK_SET);
}

static long wait_status(long child)
{
    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0);

    return wstatus;
}

static bool all_bytes_are(const volatile char* bytes, size_t count, char value)
{
    for(size_t i = 0; i < count; ++i) {
        if(bytes[i] != value) {
            return false;
        }
    }

    return true;
}

// Runs the access in a forked process, so that a fault kills only the child
// Returns the terminating signal, or zero when the access went through
static long access_kills(volatile char* address, bool store)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        if(store) {
            *address = 1;
        }
        else {
            (void)*address;
        }

        ktest_syscall1(__k_SYS_EXIT, 0);
        __builtin_unreachable();
    }

    return __k_WTERMSIG(wait_status(child));
}

static void test_anonymous(void)
{
    long base = map(0, MANY_PAGES * PAGE_SIZE, RW, PRIVATE_AN, -1, 0);

    ktest_assert(base > 0);
    ktest_expect(base % PAGE_SIZE, 0);

    volatile char* bytes = (volatile char*)(base);

    // A new page reads as zeros, wherever it is
    ktest_expect(all_bytes_are(bytes, MANY_PAGES * PAGE_SIZE, 0), 1);

    for(size_t page = 0; page < MANY_PAGES; ++page) {
        bytes[page * PAGE_SIZE] = (char)(page + 1);
        bytes[page * PAGE_SIZE + PAGE_SIZE - 1] = (char)(~page);
    }

    bool intact = true;

    for(size_t page = 0; page < MANY_PAGES; ++page) {
        intact = intact && bytes[page * PAGE_SIZE] == (char)(page + 1);
        intact = intact && bytes[page * PAGE_SIZE + PAGE_SIZE - 1] == (char)(~page);
    }

    ktest_expect(intact, 1);

    // The length rounds up to whole pages
    long odd = map(0, PAGE_SIZE + 1, RW, PRIVATE_AN, -1, 0);

    ktest_assert(odd > 0);
    ((volatile char*)(odd))[2 * PAGE_SIZE - 1] = 1;

    ktest_expect(unmap(odd, PAGE_SIZE + 1), 0);
    ktest_expect(unmap(base, MANY_PAGES * PAGE_SIZE), 0);
}

static void test_arguments(void)
{
    ktest_expect(map(0, 0, RW, PRIVATE_AN, -1, 0), -__k_EINVAL);
    ktest_expect(map(0, HUGE_LENGTH, RW, PRIVATE_AN, -1, 0), -__k_EINVAL);
    ktest_expect(map(0, PAGE_SIZE, 0x8000000, PRIVATE_AN, -1, 0), -__k_EINVAL);
    ktest_expect(map(0, PAGE_SIZE, RW, __k_MAP_ANONYMOUS, -1, 0), -__k_EINVAL);
    ktest_expect(map(0, PAGE_SIZE, RW, __k_MAP_ANONYMOUS | __k_MAP_SHARED | __k_MAP_PRIVATE, -1, 0), -__k_EINVAL);
    ktest_expect(map(0, PAGE_SIZE, RW, PRIVATE_AN | 0x40000000, -1, 0), -__k_EINVAL);

    // A fixed mapping needs an aligned address inside the user half
    ktest_expect(map(PAGE_SIZE + 1, PAGE_SIZE, RW, PRIVATE_AN | __k_MAP_FIXED, -1, 0), -__k_EINVAL);
    ktest_expect(map(-PAGE_SIZE, PAGE_SIZE, RW, PRIVATE_AN | __k_MAP_FIXED, -1, 0), -__k_EINVAL);

    // A file mapping needs a file that is open
    ktest_expect(map(0, PAGE_SIZE, RW, __k_MAP_PRIVATE, 1000, 0), -__k_EBADF);

    ktest_expect(unmap(0, 0), -__k_EINVAL);
    ktest_expect(unmap(PAGE_SIZE + 1, PAGE_SIZE), -__k_EINVAL);
    ktest_expect(protect(PAGE_SIZE + 1, PAGE_SIZE, RW), -__k_EINVAL);
    ktest_expect(protect(PAGE_SIZE, PAGE_SIZE, 0x8000000), -__k_EINVAL);
    ktest_expect(sync_range(PAGE_SIZE + 1, PAGE_SIZE, __k_MS_SYNC), -__k_EINVAL);
}

static void test_placement(void)
{
    long base = map(0, 4 * PAGE_SIZE, RW, PRIVATE_AN, -1, 0);

    ktest_assert(base > 0);

    // A free hint is taken
    ktest_expect(unmap(base, 4 * PAGE_SIZE), 0);
    ktest_expect(map(base, 4 * PAGE_SIZE, RW, PRIVATE_AN, -1, 0), base);

    // A hint on a used range is only a hint
    long moved = map(base, PAGE_SIZE, RW, PRIVATE_AN, -1, 0);

    ktest_assert(moved > 0);
    ktest_expect(moved != base, 1);
    ktest_expect(unmap(moved, PAGE_SIZE), 0);

    // Two mappings never overlap
    long first = map(0, 4 * PAGE_SIZE, RW, PRIVATE_AN, -1, 0);
    long second = map(0, 4 * PAGE_SIZE, RW, PRIVATE_AN, -1, 0);

    ktest_assert(first > 0 && second > 0);
    ktest_expect(first + 4 * PAGE_SIZE <= second || second + 4 * PAGE_SIZE <= first, 1);

    ktest_expect(unmap(first, 4 * PAGE_SIZE), 0);
    ktest_expect(unmap(second, 4 * PAGE_SIZE), 0);

    // MAP_FIXED replaces what was there, and the new pages read as zeros
    volatile char* bytes = (volatile char*)(base);

    memset((void*)(bytes), 0x55, 4 * PAGE_SIZE);
    ktest_expect(map(base + PAGE_SIZE, 2 * PAGE_SIZE, RW, PRIVATE_AN | __k_MAP_FIXED, -1, 0), base + PAGE_SIZE);
    ktest_expect(all_bytes_are(bytes, PAGE_SIZE, 0x55), 1);
    ktest_expect(all_bytes_are(bytes + PAGE_SIZE, 2 * PAGE_SIZE, 0), 1);
    ktest_expect(all_bytes_are(bytes + 3 * PAGE_SIZE, PAGE_SIZE, 0x55), 1);

    // The same range with MAP_FIXED_NOREPLACE fails and keeps the pages
    bytes[PAGE_SIZE] = 7;
    ktest_expect(map(base + PAGE_SIZE, PAGE_SIZE, RW, PRIVATE_AN | __k_MAP_FIXED_NOREPLACE, -1, 0), -__k_EEXIST);
    ktest_expect(bytes[PAGE_SIZE], 7);

    ktest_expect(unmap(base, 4 * PAGE_SIZE), 0);
}

static void test_unmap(void)
{
    long base = map(0, 4 * PAGE_SIZE, RW, PRIVATE_AN, -1, 0);

    ktest_assert(base > 0);
    memset((void*)(base), 0x33, 4 * PAGE_SIZE);

    // A hole in the middle leaves both sides mapped
    ktest_expect(unmap(base + PAGE_SIZE, 2 * PAGE_SIZE), 0);
    ktest_expect(all_bytes_are((volatile char*)(base), PAGE_SIZE, 0x33), 1);
    ktest_expect(all_bytes_are((volatile char*)(base + 3 * PAGE_SIZE), PAGE_SIZE, 0x33), 1);
    ktest_expect(access_kills((volatile char*)(base + PAGE_SIZE), false), __k_SIGSEGV);
    ktest_expect(access_kills((volatile char*)(base + 2 * PAGE_SIZE), true), __k_SIGSEGV);

    // The hole can be mapped again, and reads as zeros
    ktest_expect(map(base + PAGE_SIZE, 2 * PAGE_SIZE, RW, PRIVATE_AN | __k_MAP_FIXED_NOREPLACE, -1, 0), base + PAGE_SIZE);
    ktest_expect(all_bytes_are((volatile char*)(base + PAGE_SIZE), 2 * PAGE_SIZE, 0), 1);

    // One call can span several mappings
    ktest_expect(unmap(base, 4 * PAGE_SIZE), 0);
    ktest_expect(access_kills((volatile char*)(base), false), __k_SIGSEGV);
    ktest_expect(access_kills((volatile char*)(base + 3 * PAGE_SIZE), false), __k_SIGSEGV);
}

static void test_protect(void)
{
    long base = map(0, 3 * PAGE_SIZE, RW, PRIVATE_AN, -1, 0);

    ktest_assert(base > 0);
    memset((void*)(base), 0x11, 3 * PAGE_SIZE);

    // Only the middle page turns read-only
    ktest_expect(protect(base + PAGE_SIZE, PAGE_SIZE, __k_PROT_READ), 0);
    ktest_expect(access_kills((volatile char*)(base + PAGE_SIZE), true), __k_SIGSEGV);
    ktest_expect(access_kills((volatile char*)(base + PAGE_SIZE), false), 0);
    ktest_expect(access_kills((volatile char*)(base), true), 0);
    ktest_expect(access_kills((volatile char*)(base + 2 * PAGE_SIZE), true), 0);
    ktest_expect(all_bytes_are((volatile char*)(base + PAGE_SIZE), PAGE_SIZE, 0x11), 1);

    ktest_expect(protect(base + PAGE_SIZE, PAGE_SIZE, __k_PROT_NONE), 0);
    ktest_expect(access_kills((volatile char*)(base + PAGE_SIZE), false), __k_SIGSEGV);

    // The data survives a round trip
    ktest_expect(protect(base + PAGE_SIZE, PAGE_SIZE, RW), 0);
    ktest_expect(all_bytes_are((volatile char*)(base + PAGE_SIZE), PAGE_SIZE, 0x11), 1);
    ktest_expect(access_kills((volatile char*)(base + PAGE_SIZE), true), 0);

    // A mapping made read-only from the start
    long readonly = map(0, PAGE_SIZE, __k_PROT_READ, PRIVATE_AN, -1, 0);

    ktest_assert(readonly > 0);
    ktest_expect(all_bytes_are((volatile char*)(readonly), PAGE_SIZE, 0), 1);
    ktest_expect(access_kills((volatile char*)(readonly), true), __k_SIGSEGV);

    ktest_expect(unmap(base, 3 * PAGE_SIZE), 0);
    ktest_expect(unmap(readonly, PAGE_SIZE), 0);
}

static long make_file(const char* path)
{
    long fd = ktest_open(path, __k_O_RDWR | __k_O_CREAT, 0644);

    ktest_assert(fd >= 0);

    char page[PAGE_SIZE];

    for(int index = 0; index < FILE_PAGES; ++index) {
        memset(page, 'A' + index, sizeof(page));
        ktest_expect(ktest_write(fd, page, sizeof(page)), PAGE_SIZE);
    }

    return fd;
}

static void test_file_private(long fd)
{
    long base = map(0, FILE_SIZE, RW, __k_MAP_PRIVATE, fd, 0);

    ktest_assert(base > 0);

    volatile char* bytes = (volatile char*)(base);

    ktest_expect(all_bytes_are(bytes, PAGE_SIZE, 'A'), 1);
    ktest_expect(all_bytes_are(bytes + PAGE_SIZE, PAGE_SIZE, 'B'), 1);
    ktest_expect(all_bytes_are(bytes + 2 * PAGE_SIZE, PAGE_SIZE, 'C'), 1);

    // A write goes to a copy: the neighbours and the file keep their bytes
    bytes[PAGE_SIZE] = 'x';
    bytes[PAGE_SIZE + 1] = 'y';

    char check[2];

    ktest_expect(seek_to(fd, PAGE_SIZE), PAGE_SIZE);
    ktest_expect(ktest_read(fd, check, sizeof(check)), 2);
    ktest_expect(check[0], 'B');
    ktest_expect(check[1], 'B');
    ktest_expect(bytes[PAGE_SIZE + 2], 'B');
    ktest_expect(bytes[0], 'A');

    ktest_expect(unmap(base, FILE_SIZE), 0);

    // An offset skips pages, and a map outlives the descriptor
    long tail = map(0, PAGE_SIZE, __k_PROT_READ, __k_MAP_PRIVATE, fd, 2 * PAGE_SIZE);

    ktest_assert(tail > 0);
    ktest_expect(all_bytes_are((volatile char*)(tail), PAGE_SIZE, 'C'), 1);

    ktest_expect(map(0, PAGE_SIZE, __k_PROT_READ, __k_MAP_PRIVATE, fd, 1), -__k_EINVAL);
    ktest_expect(unmap(tail, PAGE_SIZE), 0);
}

static void test_file_shared(long fd)
{
    long base = map(0, FILE_SIZE, RW, __k_MAP_SHARED, fd, 0);

    ktest_assert(base > 0);

    volatile char* bytes = (volatile char*)(base);

    // A write through the mapping shows in the file, and a write to the file shows in the mapping
    bytes[10] = 'm';
    bytes[2 * PAGE_SIZE + 10] = 'n';

    char check[1];

    ktest_expect(seek_to(fd, 10), 10);
    ktest_expect(ktest_read(fd, check, 1), 1);
    ktest_expect(check[0], 'm');

    ktest_expect(seek_to(fd, 2 * PAGE_SIZE + 10), 2 * PAGE_SIZE + 10);
    ktest_expect(ktest_read(fd, check, 1), 1);
    ktest_expect(check[0], 'n');

    ktest_expect(seek_to(fd, PAGE_SIZE + 20), PAGE_SIZE + 20);
    ktest_expect(ktest_write(fd, "z", 1), 1);
    ktest_expect(bytes[PAGE_SIZE + 20], 'z');

    // A second mapping of the same page sees the same bytes
    long again = map(0, PAGE_SIZE, RW, __k_MAP_SHARED, fd, 0);

    ktest_assert(again > 0);
    ktest_expect(((volatile char*)(again))[10], 'm');
    ((volatile char*)(again))[11] = 'q';
    ktest_expect(bytes[11], 'q');
    ktest_expect(unmap(again, PAGE_SIZE), 0);

    ktest_expect(sync_range(base, FILE_SIZE, __k_MS_SYNC), 0);
    ktest_expect(sync_range(base, FILE_SIZE, __k_MS_ASYNC), 0);
    ktest_expect(sync_range(base, FILE_SIZE, __k_MS_SYNC | __k_MS_ASYNC), -__k_EINVAL);
    ktest_expect(sync_range(base, FILE_SIZE, 0x80), -__k_EINVAL);

    ktest_expect(unmap(base, FILE_SIZE), 0);
}

static void test_file_access(const char* path)
{
    long readonly = ktest_open(path, __k_O_RDONLY, 0);

    ktest_assert(readonly >= 0);

    // A shared writable mapping needs a descriptor that can write
    ktest_expect(map(0, PAGE_SIZE, RW, __k_MAP_SHARED, readonly, 0), -__k_EACCES);

    // A private one only needs to read
    long copy = map(0, PAGE_SIZE, RW, __k_MAP_PRIVATE, readonly, 0);

    ktest_assert(copy > 0);
    ((volatile char*)(copy))[0] = 'k';
    ktest_expect(unmap(copy, PAGE_SIZE), 0);

    long shared = map(0, PAGE_SIZE, __k_PROT_READ, __k_MAP_SHARED, readonly, 0);

    ktest_assert(shared > 0);
    ktest_expect(access_kills((volatile char*)(shared), true), __k_SIGSEGV);
    ktest_expect(unmap(shared, PAGE_SIZE), 0);
    ktest_close(readonly);

    long writeonly = ktest_open(path, __k_O_WRONLY, 0);

    ktest_assert(writeonly >= 0);
    ktest_expect(map(0, PAGE_SIZE, __k_PROT_READ, __k_MAP_PRIVATE, writeonly, 0), -__k_EACCES);
    ktest_close(writeonly);

    // Only files can be mapped
    long directory = ktest_open(".", __k_O_RDONLY, 0);

    ktest_assert(directory >= 0);
    ktest_expect(map(0, PAGE_SIZE, __k_PROT_READ, __k_MAP_PRIVATE, directory, 0), -__k_ENODEV);
    ktest_close(directory);
}

static void test_fork(long fd)
{
    long anonymous = map(0, PAGE_SIZE, RW, PRIVATE_AN, -1, 0);
    long shared = map(0, PAGE_SIZE, RW, __k_MAP_SHARED, fd, 0);
    long private_file = map(0, PAGE_SIZE, RW, __k_MAP_PRIVATE, fd, PAGE_SIZE);

    ktest_assert(anonymous > 0 && shared > 0 && private_file > 0);

    volatile char* anonymous_bytes = (volatile char*)(anonymous);
    volatile char* shared_bytes = (volatile char*)(shared);
    volatile char* private_bytes = (volatile char*)(private_file);

    anonymous_bytes[0] = 'p';
    shared_bytes[100] = 'p';
    private_bytes[0] = 'p';

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        // The child starts with the same bytes
        bool ok = anonymous_bytes[0] == 'p' && shared_bytes[100] == 'p' && private_bytes[0] == 'p';

        anonymous_bytes[0] = 'c';
        private_bytes[0] = 'c';
        shared_bytes[100] = 'c';

        ktest_syscall1(__k_SYS_EXIT, ok ? 3 : 4);
        __builtin_unreachable();
    }

    ktest_expect(wait_status(child), __k_W_EXITCODE(3, 0));

    // Private pages stay private, shared ones are shared
    ktest_expect(anonymous_bytes[0], 'p');
    ktest_expect(private_bytes[0], 'p');
    ktest_expect(shared_bytes[100], 'c');

    // The parent writing after the fork does not reach the child either
    long late_child = ktest_syscall1(__k_SYS_FORK, 0);

    if(late_child == 0) {
        for(int i = 0; i < 200; ++i) {
            ktest_syscall1(__k_SYS_YIELD, 0);
        }

        ktest_syscall1(__k_SYS_EXIT, anonymous_bytes[0] == 'p' ? 5 : 6);
        __builtin_unreachable();
    }

    anonymous_bytes[0] = 'q';
    ktest_expect(wait_status(late_child), __k_W_EXITCODE(5, 0));

    ktest_expect(unmap(anonymous, PAGE_SIZE), 0);
    ktest_expect(unmap(shared, PAGE_SIZE), 0);
    ktest_expect(unmap(private_file, PAGE_SIZE), 0);
}

static void test_populate_and_stack(void)
{
    long populated = map(0, MANY_PAGES * PAGE_SIZE, RW, PRIVATE_AN | __k_MAP_POPULATE, -1, 0);

    ktest_assert(populated > 0);
    ktest_expect(all_bytes_are((volatile char*)(populated), MANY_PAGES * PAGE_SIZE, 0), 1);
    ktest_expect(unmap(populated, MANY_PAGES * PAGE_SIZE), 0);

    // A file map can populate too, and sees the file
    long fd = make_file("populate.bin");
    long file = map(0, FILE_SIZE, __k_PROT_READ, __k_MAP_PRIVATE | __k_MAP_POPULATE, fd, 0);

    ktest_assert(file > 0);
    ktest_expect(((volatile char*)(file))[0], 'A');
    ktest_expect(((volatile char*)(file))[2 * PAGE_SIZE], 'C');
    ktest_expect(unmap(file, FILE_SIZE), 0);
    ktest_close(fd);

    // Flags that change nothing are accepted together with the others
    long plain = map(0, PAGE_SIZE, RW, PRIVATE_AN | __k_MAP_NORESERVE | __k_MAP_STACK, -1, 0);

    ktest_assert(plain > 0);
    ((volatile char*)(plain))[0] = 1;
    ktest_expect(unmap(plain, PAGE_SIZE), 0);
}

int main(void)
{
    ktest_enter_work_directory("mmap");

    test_anonymous();
    test_arguments();
    test_placement();
    test_unmap();
    test_protect();
    test_populate_and_stack();

    long fd = make_file("data.bin");

    test_file_private(fd);
    test_file_shared(fd);
    test_fork(fd);
    ktest_close(fd);
    test_file_access("data.bin");

    return ktest_result();
}
