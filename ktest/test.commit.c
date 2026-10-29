#include <vex/capability.h>
#include <vex/errno.h>
#include <vex/mman.h>
#include <vex/syscall.h>

#include "ktest.h"

#define PAGE_SIZE   4096
#define MAX_PAGES   (1L << 32) // 16 TiB, below the top of the range that mmap picks from
#define SPARE_PAGES 4
#define RW          (__k_PROT_READ | __k_PROT_WRITE)
#define ANONYMOUS   (__k_MAP_PRIVATE | __k_MAP_ANONYMOUS)
#define NORESERVE   (ANONYMOUS | __k_MAP_NORESERVE)

static long map_anonymous(long address, long pages, long prot, long flags)
{
    return ktest_syscall6(__k_SYS_MMAP, address, pages * PAGE_SIZE, prot, flags, -1, 0);
}

static long map_writable(long pages)
{
    return map_anonymous(0, pages, RW, ANONYMOUS);
}

static long unmap(long address, long pages)
{
    return ktest_syscall2(__k_SYS_MUNMAP, address, pages * PAGE_SIZE);
}

static long protect(long address, long pages, long prot)
{
    return ktest_syscall3(__k_SYS_MPROTECT, address, pages * PAGE_SIZE, prot);
}

// The most pages that one mapping of private writable memory can take now
static long free_pages(void)
{
    long low = 0;
    long high = MAX_PAGES;

    while(low < high) {
        long middle = low + (high - low + 1) / 2;
        long address = map_writable(middle);

        if(address > 0) {
            unmap(address, middle);
            low = middle;
        }
        else {
            high = middle - 1;
        }
    }

    return low;
}

static void test_limit(long room)
{
    ktest_expect(map_writable(room + 1), -__k_ENOMEM);

    long all = map_writable(room);

    ktest_assert(all > 0);
    ktest_expect(map_writable(1), -__k_ENOMEM);
    ktest_expect(free_pages(), 0);

    // Memory that no write can fill takes nothing
    long reserved = map_anonymous(0, room, __k_PROT_NONE, ANONYMOUS);
    long readable = map_anonymous(0, room, __k_PROT_READ, ANONYMOUS);

    ktest_assert(reserved > 0 && readable > 0);
    ktest_expect(free_pages(), 0);

    ktest_expect(unmap(all, room), 0);
    ktest_expect(free_pages(), room);

    ktest_expect(unmap(reserved, room), 0);
    ktest_expect(unmap(readable, room), 0);
}

static void test_unmap_part(long room)
{
    long all = map_writable(room);

    ktest_assert(all > 0);

    // A hole in the middle gives its pages back, and the two halves go on counting
    ktest_expect(unmap(all + PAGE_SIZE, 2), 0);
    ktest_expect(free_pages(), 2);

    ktest_expect(unmap(all, 1), 0);
    ktest_expect(unmap(all + 3 * PAGE_SIZE, room - 3), 0);
    ktest_expect(free_pages(), room);
}

static void test_protect(long room)
{
    long hold = map_writable(room - SPARE_PAGES);
    long area = map_anonymous(0, 2 * SPARE_PAGES, __k_PROT_NONE, ANONYMOUS);
    long upper = area + SPARE_PAGES * PAGE_SIZE;

    ktest_assert(hold > 0 && area > 0);
    ktest_expect(free_pages(), SPARE_PAGES);

    ktest_expect(protect(area, SPARE_PAGES, RW), 0);
    ktest_expect(free_pages(), 0);
    ktest_expect(protect(upper, SPARE_PAGES, RW), -__k_ENOMEM);
    ktest_expect(free_pages(), 0);

    // Read only gives the pages back
    ktest_expect(protect(area, SPARE_PAGES, __k_PROT_READ), 0);
    ktest_expect(free_pages(), SPARE_PAGES);
    ktest_expect(protect(upper, SPARE_PAGES, RW), 0);
    ktest_expect(free_pages(), 0);
    ktest_expect(protect(upper, SPARE_PAGES, __k_PROT_READ), 0);
    ktest_expect(free_pages(), SPARE_PAGES);

    // The call changes all of the range or none of it
    ktest_expect(protect(area, 2 * SPARE_PAGES, RW), -__k_ENOMEM);
    ktest_expect(free_pages(), SPARE_PAGES);

    ktest_expect(unmap(area, 2 * SPARE_PAGES), 0);
    ktest_expect(unmap(hold, room - SPARE_PAGES), 0);
    ktest_expect(free_pages(), room);
}

static void test_replace(long room)
{
    long hold = map_writable(room - SPARE_PAGES);
    long area = map_anonymous(0, 2 * SPARE_PAGES, __k_PROT_NONE, ANONYMOUS);
    long flags = ANONYMOUS | __k_MAP_FIXED;

    ktest_assert(hold > 0 && area > 0);

    // The memory under it has no access, so it takes all the new pages
    ktest_expect(map_anonymous(area, SPARE_PAGES, RW, flags), area);
    ktest_expect(free_pages(), 0);

    // Memory that takes the place of as much memory needs no room
    ktest_expect(map_anonymous(area, SPARE_PAGES, RW, flags), area);
    ktest_expect(free_pages(), 0);

    // Twice as much does not fit, and the old mapping keeps its pages
    ktest_expect(map_anonymous(area, 2 * SPARE_PAGES, RW, flags), -__k_ENOMEM);
    ktest_expect(free_pages(), 0);

    ktest_expect(map_anonymous(area, SPARE_PAGES, __k_PROT_NONE, flags), area);
    ktest_expect(free_pages(), SPARE_PAGES);

    ktest_expect(unmap(area, 2 * SPARE_PAGES), 0);
    ktest_expect(unmap(hold, room - SPARE_PAGES), 0);
    ktest_expect(free_pages(), room);
}

static long fork_and_exit(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    return child;
}

static void reap(long child)
{
    int wstatus = -1;

    ktest_assert(child > 0);
    ktest_expect(ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0), child);
    ktest_expect(wstatus, 0);
}

static void test_fork(long room)
{
    long hold = map_writable(room - SPARE_PAGES);

    ktest_assert(hold > 0);

    // The child would hold a copy of all of it, and a failed fork leaves nothing behind
    ktest_expect(fork_and_exit(), -__k_ENOMEM);
    ktest_expect(free_pages(), SPARE_PAGES);
    ktest_expect(unmap(hold, room - SPARE_PAGES), 0);

    long quarter = room / 4;

    hold = map_writable(quarter);
    ktest_assert(hold > 0);

    reap(fork_and_exit());

    // The copy of the child goes with it
    ktest_expect(free_pages(), room - quarter);
    ktest_expect(unmap(hold, quarter), 0);
}

static void test_noreserve(long room)
{
    // The memory takes nothing, so there can be more of it than all that exists
    long big = map_anonymous(0, 2 * room, RW, NORESERVE);

    ktest_assert(big > 0);
    ktest_expect(free_pages(), room);

    // A change of the rights does not make it count
    long none = map_anonymous(0, room, __k_PROT_NONE, NORESERVE);

    ktest_assert(none > 0);
    ktest_expect(protect(none, room, RW), 0);
    ktest_expect(protect(none, room, __k_PROT_READ), 0);
    ktest_expect(protect(none, room, RW), 0);
    ktest_expect(free_pages(), room);

    // A hole in it does not give back pages that it never took
    ktest_expect(unmap(big + PAGE_SIZE, 1), 0);
    ktest_expect(free_pages(), room);

    // A child needs no room for the copy
    reap(fork_and_exit());
    ktest_expect(free_pages(), room);

    ktest_expect(unmap(big, 1), 0);
    ktest_expect(unmap(big + 2 * PAGE_SIZE, 2 * room - 2), 0);
    ktest_expect(unmap(none, room), 0);
    ktest_expect(free_pages(), room);
}

static void test_noreserve_denied(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_caps caps;

        ktest_syscall1(__k_SYS_CAPGET, (long)(&caps));
        caps.effective &= ~__k_CAP_TO_MASK(__k_CAP_VM_NORESERVE);
        ktest_expect(ktest_syscall1(__k_SYS_CAPSET, (long)(&caps)), 0);

        // The flag does nothing then, and the pages count as for any other mapping
        long room = free_pages();

        ktest_expect(map_anonymous(0, room + 1, RW, NORESERVE), -__k_ENOMEM);

        long half = map_anonymous(0, room / 2, RW, NORESERVE);

        ktest_assert(half > 0);
        ktest_expect(free_pages(), room - room / 2);
        ktest_expect(unmap(half, room / 2), 0);
        ktest_expect(free_pages(), room);

        ktest_exit(ktest_result());
    }

    reap(child);
}

int main(void)
{
    long room = free_pages();

    ktest_assert(room > 4 * SPARE_PAGES);

    test_limit(room);
    test_unmap_part(room);
    test_protect(room);
    test_replace(room);
    test_fork(room);
    test_noreserve(room);
    test_noreserve_denied();

    ktest_expect(free_pages(), room);

    return ktest_result();
}
