#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/mman.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define PAGE_SIZE   4096L
#define RW          (__k_PROT_READ | __k_PROT_WRITE)
#define PRIVATE     (__k_MAP_PRIVATE | __k_MAP_ANONYMOUS)
#define SHARED      (__k_MAP_SHARED | __k_MAP_ANONYMOUS)
#define BLOCKER     (__k_MAP_PRIVATE | __k_MAP_ANONYMOUS | __k_MAP_FIXED_NOREPLACE)
#define FILE_MODE   0644
#define HUGE_SIZE   (1L << 44)
#define SETTLE_NSEC 20000000L
#define POLL_COUNT  200

static long map_pages(long pages, long prot, long flags)
{
    return ktest_syscall6(__k_SYS_MMAP, 0, pages * PAGE_SIZE, prot, flags, -1, 0);
}

static long remap(long address, long old_pages, long new_pages, long flags)
{
    return ktest_syscall4(__k_SYS_MREMAP, address, old_pages * PAGE_SIZE, new_pages * PAGE_SIZE, flags);
}

static long unmap_pages(long address, long pages)
{
    return ktest_syscall2(__k_SYS_MUNMAP, address, pages * PAGE_SIZE);
}

static bool is_mapped(long address)
{
    return ktest_syscall3(__k_SYS_MADVISE, address, PAGE_SIZE, __k_MADV_NORMAL) == 0;
}

static volatile char* page_at(long address, long page)
{
    return (volatile char*)(address + page * PAGE_SIZE);
}

static void fill(long address, long pages, char first)
{
    for(long page = 0; page < pages; ++page) {
        *page_at(address, page) = first + page;
    }
}

static bool holds(long address, long pages, char first)
{
    for(long page = 0; page < pages; ++page) {
        if(*page_at(address, page) != (char)(first + page)) {
            return false;
        }
    }

    return true;
}

static bool zeroed(long address, long from, long to)
{
    for(long page = from; page < to; ++page) {
        if(*page_at(address, page)) {
            return false;
        }
    }

    return true;
}

// A mapping of the given size with free address space right after it
static long map_with_room(long pages, long room)
{
    long address = map_pages(pages + room, RW, PRIVATE);

    ktest_expect(address > 0, 1);
    ktest_expect(unmap_pages(address + pages * PAGE_SIZE, room), 0);

    return address;
}

// Maps over the start of a fresh range, and gives back the rest, so that free address space follows the mapping
static long map_over_room(long pages, long room, long prot, long flags, long fd)
{
    long address = map_pages(pages + room, RW, PRIVATE);

    ktest_expect(address > 0, 1);
    ktest_expect(ktest_syscall6(__k_SYS_MMAP, address, pages * PAGE_SIZE, prot, flags | __k_MAP_FIXED, fd, 0), address);
    ktest_expect(unmap_pages(address + pages * PAGE_SIZE, room), 0);

    return address;
}

// Takes the page after the mapping, so that it cannot grow in place
static void block_after(long address, long pages)
{
    ktest_expect(ktest_syscall6(__k_SYS_MMAP, address + pages * PAGE_SIZE, PAGE_SIZE, RW, BLOCKER, -1, 0), address + pages * PAGE_SIZE);
}

static void test_in_place(void)
{
    long address = map_with_room(2, 4);

    fill(address, 2, 'a');

    // The same size changes nothing
    ktest_expect(remap(address, 2, 2, 0), address);

    // Growing into the free space after it keeps the address and the contents, and the new pages read as zeroes
    ktest_expect(remap(address, 2, 4, 0), address);
    ktest_expect(holds(address, 2, 'a'), 1);
    ktest_expect(zeroed(address, 2, 4), 1);
    fill(address, 4, 'k');

    // Shrinking cuts the end
    ktest_expect(remap(address, 4, 1, 0), address);
    ktest_expect(holds(address, 1, 'k'), 1);
    ktest_expect(is_mapped(address), 1);
    ktest_expect(is_mapped(address + PAGE_SIZE), 0);

    // The cut part is free again
    ktest_expect(remap(address, 1, 3, 0), address);
    ktest_expect(zeroed(address, 1, 3), 1);

    ktest_expect(unmap_pages(address, 3), 0);
}

static void test_move(void)
{
    long address = map_with_room(2, 4);

    fill(address, 2, 'm');
    block_after(address, 2);

    // Without MAYMOVE it cannot grow past the blocker
    ktest_expect(remap(address, 2, 4, 0), -__k_ENOMEM);
    ktest_expect(holds(address, 2, 'm'), 1);

    long moved = remap(address, 2, 4, __k_MREMAP_MAYMOVE);

    ktest_expect(moved > 0, 1);
    ktest_expect(moved != address, 1);
    ktest_expect(holds(moved, 2, 'm'), 1);
    ktest_expect(zeroed(moved, 2, 4), 1);
    ktest_expect(is_mapped(address), 0);
    ktest_expect(is_mapped(address + PAGE_SIZE), 0);
    ktest_expect(is_mapped(address + 2 * PAGE_SIZE), 1);

    fill(moved, 4, 'n');
    ktest_expect(holds(moved, 4, 'n'), 1);

    ktest_expect(unmap_pages(moved, 4), 0);
    ktest_expect(unmap_pages(address + 2 * PAGE_SIZE, 1), 0);
}

// A page that a write made private moves with its contents, and one that is still the file's goes on being the file's
static void test_private_file(void)
{
    static char page[PAGE_SIZE];

    long fd = ktest_open("file", __k_O_CREAT | __k_O_RDWR, FILE_MODE);

    ktest_expect(fd >= 0, 1);

    for(long i = 0; i < PAGE_SIZE; ++i) {
        page[i] = 'F';
    }

    ktest_expect(ktest_syscall4(__k_SYS_PWRITE, fd, (long)(page), PAGE_SIZE, 0), PAGE_SIZE);
    ktest_expect(ktest_syscall4(__k_SYS_PWRITE, fd, (long)(page), PAGE_SIZE, PAGE_SIZE), PAGE_SIZE);

    long address = map_over_room(2, 2, RW, __k_MAP_PRIVATE, fd);

    *page_at(address, 0) = 'X';
    ktest_expect(*page_at(address, 1), 'F');

    block_after(address, 2);

    long moved = remap(address, 2, 3, __k_MREMAP_MAYMOVE);

    ktest_expect(moved > 0 && moved != address, 1);
    ktest_expect(*page_at(moved, 0), 'X');
    ktest_expect(*page_at(moved, 1), 'F');

    // Beyond the end of the file is a bus error, so the new page is left alone. A write to the copy stays private
    *page_at(moved, 1) = 'Y';
    ktest_expect(ktest_syscall4(__k_SYS_PREAD, fd, (long)(page), 1, PAGE_SIZE), 1);
    ktest_expect(page[0], 'F');
    ktest_expect(ktest_syscall4(__k_SYS_PREAD, fd, (long)(page), 1, 0), 1);
    ktest_expect(page[0], 'F');

    ktest_expect(unmap_pages(moved, 3), 0);
    ktest_expect(unmap_pages(address + 2 * PAGE_SIZE, 1), 0);
    ktest_close(fd);
    ktest_expect(ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)("file"), 0), 0);
}

// The mapping of the child is the old one, and shares the memory with the new one
static void test_shared(void)
{
    long address = map_over_room(1, 2, RW, SHARED, -1);

    *page_at(address, 0) = 'S';

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_timespec nap = {};
        nap.tv_nsec = SETTLE_NSEC;

        for(int i = 0; i < POLL_COUNT; ++i) {
            if(*page_at(address, 0) == 'Q') {
                ktest_syscall1(__k_SYS_EXIT, 0);
            }

            ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&nap), 0);
        }

        ktest_syscall1(__k_SYS_EXIT, 1);
    }

    block_after(address, 1);

    long moved = remap(address, 1, 2, __k_MREMAP_MAYMOVE);

    ktest_expect(moved > 0 && moved != address, 1);
    ktest_expect(*page_at(moved, 0), 'S');
    *page_at(moved, 0) = 'Q';

    int wstatus = -1;

    ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, 0);
    ktest_expect(wstatus, 0);

    ktest_expect(unmap_pages(moved, 2), 0);
    ktest_expect(unmap_pages(address + PAGE_SIZE, 1), 0);
}

static void test_commit(void)
{
    long address = map_with_room(2, 4);

    fill(address, 2, 'c');

    // A failed call leaves the mapping as it was, for growing in place and for moving
    ktest_expect(remap(address, 2, HUGE_SIZE / PAGE_SIZE, 0), -__k_ENOMEM);
    ktest_expect(remap(address, 2, HUGE_SIZE / PAGE_SIZE, __k_MREMAP_MAYMOVE), -__k_ENOMEM);
    ktest_expect(holds(address, 2, 'c'), 1);
    ktest_expect(remap(address, 2, 3, 0), address);
    ktest_expect(unmap_pages(address, 3), 0);

    // Nothing was kept from the failures: a new mapping still fits after them
    long big = map_pages(2, RW, PRIVATE);

    ktest_expect(big > 0, 1);
    ktest_expect(unmap_pages(big, 2), 0);
}

static void test_errors(void)
{
    long address = map_with_room(2, 4);

    ktest_expect(remap(address + 1, 2, 3, 0), -__k_EINVAL);
    ktest_expect(remap(address, 0, 3, 0), -__k_EINVAL);
    ktest_expect(remap(address, 2, 0, 0), -__k_EINVAL);
    ktest_expect(remap(address, 2, 3, 2), -__k_EINVAL);
    ktest_expect(remap(address, 2, 3, 4), -__k_EINVAL);
    ktest_expect(remap(address, 2, 3, -1), -__k_EINVAL);

    // Not mapped, or not all of it
    ktest_expect(remap(address + 4 * PAGE_SIZE, 1, 2, 0), -__k_EFAULT);
    ktest_expect(remap(address, 3, 4, 0), -__k_EFAULT);

    // A range over two mappings: a change of protection splits the memory in two
    ktest_expect(ktest_syscall3(__k_SYS_MPROTECT, address + PAGE_SIZE, PAGE_SIZE, __k_PROT_READ), 0);
    ktest_expect(remap(address, 2, 3, 0), -__k_EFAULT);
    ktest_expect(remap(address, 1, 2, 0), -__k_ENOMEM);

    ktest_expect(unmap_pages(address, 2), 0);
}

int main(void)
{
    ktest_enter_work_directory("mremap");

    test_in_place();
    test_move();
    test_private_file();
    test_shared();
    test_commit();
    test_errors();

    return ktest_result();
}
