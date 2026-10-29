#include <vex/errno.h>
#include <vex/futex.h>
#include <vex/mman.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define PAGE_SIZE    4096
#define MAX_PAGES    (1L << 32) // 16 TiB, below the top of the range that mmap picks from
#define TEST_PAGES   4
#define GIVE_UP_NSEC 5000000000L
#define SETTLE_NSEC  50000000L
#define RW           (__k_PROT_READ | __k_PROT_WRITE)
#define PRIVATE      (__k_MAP_PRIVATE | __k_MAP_ANONYMOUS)
#define SHARED       (__k_MAP_SHARED | __k_MAP_ANONYMOUS)

static long map_memory(long address, long pages, long prot, long flags)
{
    return ktest_syscall6(__k_SYS_MMAP, address, pages * PAGE_SIZE, prot, flags, -1, 0);
}

static long unmap(long address, long pages)
{
    return ktest_syscall2(__k_SYS_MUNMAP, address, pages * PAGE_SIZE);
}

// The most pages that one mapping of private writable memory can take now
static long free_pages(void)
{
    long low = 0;
    long high = MAX_PAGES;

    while(low < high) {
        long middle = low + (high - low + 1) / 2;
        long address = map_memory(0, middle, RW, PRIVATE);

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

static long wait_status(long child)
{
    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, 0);

    return wstatus;
}

static void sleep_for(long nanoseconds)
{
    struct __k_timespec request;
    request.tv_sec = nanoseconds / 1000000000L;
    request.tv_nsec = nanoseconds % 1000000000L;

    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)&request, 0);
}

static void test_sharing(void)
{
    long base = map_memory(0, TEST_PAGES, RW, SHARED);

    ktest_assert(base > 0);

    volatile char* memory = (volatile char*)(base);

    // New memory is zeroed, in every page
    for(long page = 0; page < TEST_PAGES; ++page) {
        ktest_expect(memory[page * PAGE_SIZE], 0);
    }

    // Both sides write through their own copy of the mapping, and both see the other. A private page would stay as it was
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        memory[0] = 'c';
        memory[(TEST_PAGES - 1) * PAGE_SIZE] = 'd';
        sleep_for(SETTLE_NSEC);
        ktest_syscall1(__k_SYS_EXIT, memory[1] == 'p' ? 7 : 6);
    }

    memory[1] = 'p';
    ktest_expect(wait_status(child), __k_W_EXITCODE(7, 0));
    ktest_expect(memory[0], 'c');
    ktest_expect(memory[(TEST_PAGES - 1) * PAGE_SIZE], 'd');

    ktest_expect(unmap(base, TEST_PAGES), 0);
}

static void test_futex(void)
{
    long base = map_memory(0, 1, RW, SHARED);

    ktest_assert(base > 0);

    volatile unsigned int* word = (volatile unsigned int*)(base);
    struct __k_timespec timeout;
    timeout.tv_sec = GIVE_UP_NSEC / 1000000000L;
    timeout.tv_nsec = 0;

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        while(*word == 0) {
            long retval = ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)word, 0, (long)&timeout, __k_FUTEX_SHARED);

            if(retval == -__k_ETIMEDOUT) {
                ktest_syscall1(__k_SYS_EXIT, 6);
            }
        }

        ktest_syscall1(__k_SYS_EXIT, 7);
    }

    sleep_for(SETTLE_NSEC);
    *word = 1;
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)word, 1, __k_FUTEX_SHARED) >= 0, 1);
    ktest_expect(wait_status(child), __k_W_EXITCODE(7, 0));

    ktest_expect(unmap(base, 1), 0);
}

static void test_protection(void)
{
    long base = map_memory(0, 1, __k_PROT_READ, SHARED);

    ktest_assert(base > 0);

    // The memory can be made writable later, as the file under it is open for writing
    ktest_expect(ktest_syscall3(__k_SYS_MPROTECT, base, PAGE_SIZE, RW), 0);

    *(volatile char*)(base) = 'w';
    ktest_expect(*(volatile char*)(base), 'w');
    ktest_expect(unmap(base, 1), 0);
}

static void test_commit(long room)
{
    ktest_expect(map_memory(0, room + 1, RW, SHARED), -__k_ENOMEM);
    ktest_expect(free_pages(), room);

    // Shared memory takes its whole size at once, with no page written
    long base = map_memory(0, TEST_PAGES, RW, SHARED);

    ktest_assert(base > 0);
    ktest_expect(free_pages(), room - TEST_PAGES);

    // The pages stay with the file while any part of the mapping is left
    ktest_expect(unmap(base, TEST_PAGES / 2), 0);
    ktest_expect(free_pages(), room - TEST_PAGES);
    ktest_expect(unmap(base + (TEST_PAGES / 2) * PAGE_SIZE, TEST_PAGES - TEST_PAGES / 2), 0);
    ktest_expect(free_pages(), room);

    // A call that fails after the file exists gives the pages back
    base = map_memory(0, TEST_PAGES, RW, SHARED);
    ktest_assert(base > 0);
    ktest_expect(map_memory(base, TEST_PAGES, RW, SHARED | __k_MAP_FIXED_NOREPLACE), -__k_EEXIST);
    ktest_expect(free_pages(), room - TEST_PAGES);
    ktest_expect(unmap(base, TEST_PAGES), 0);

    // The mapping is the last owner of the file, and so is a process that never unmaps
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        ktest_syscall1(__k_SYS_EXIT, map_memory(0, TEST_PAGES, RW, SHARED) > 0 ? 7 : 6);
    }

    ktest_expect(wait_status(child), __k_W_EXITCODE(7, 0));
    ktest_expect(free_pages(), room);

    // A child keeps the memory of its parent alive. The child has private memory of its own that counts too,
    // so the pages are compared with what is free while both hold the mapping
    base = map_memory(0, TEST_PAGES, RW, SHARED);

    long gate_base = map_memory(0, 1, RW, SHARED);

    ktest_assert(base > 0 && gate_base > 0);

    volatile unsigned int* gate = (volatile unsigned int*)(gate_base);
    struct __k_timespec timeout;
    timeout.tv_sec = GIVE_UP_NSEC / 1000000000L;
    timeout.tv_nsec = 0;
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        while(*gate == 0) {
            ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)gate, 0, (long)&timeout, __k_FUTEX_SHARED);
        }

        ktest_syscall1(__k_SYS_EXIT, 7);
    }

    long both_hold = free_pages();

    ktest_expect(unmap(base, TEST_PAGES), 0);
    ktest_expect(free_pages(), both_hold);

    *gate = 1;
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)gate, 1, __k_FUTEX_SHARED) >= 0, 1);
    ktest_expect(wait_status(child), __k_W_EXITCODE(7, 0));
    ktest_expect(unmap(gate_base, 1), 0);
    ktest_expect(free_pages(), room);

    // All the room goes to shared memory, and then private memory has none
    base = map_memory(0, room, RW, SHARED);
    ktest_assert(base > 0);
    ktest_expect(map_memory(0, 1, RW, PRIVATE), -__k_ENOMEM);
    ktest_expect(unmap(base, room), 0);
    ktest_expect(free_pages(), room);
}

int main(void)
{
    test_sharing();
    test_futex();
    test_protection();
    test_commit(free_pages());

    return ktest_result();
}
