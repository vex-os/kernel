#include <vex/errno.h>
#include <vex/mman.h>
#include <vex/syscall.h>

#include "ktest.h"

#define PAGE_SIZE 4096
#define LENGTH    (4 * PAGE_SIZE)

static long map_anonymous(long address, long flags)
{
    return ktest_syscall6(__k_SYS_MMAP, address, LENGTH, __k_PROT_READ | __k_PROT_WRITE, __k_MAP_PRIVATE | __k_MAP_ANONYMOUS | flags, -1,
        0);
}

int main(void)
{
    // Flags that need no work from the kernel
    long plain = map_anonymous(0, __k_MAP_NORESERVE | __k_MAP_STACK);

    ktest_expect(plain > 0, 1);

    if(plain > 0) {
        *(volatile char*)(plain) = 1;
        ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, plain, LENGTH), 0);
    }

    long populated = map_anonymous(0, __k_MAP_POPULATE);

    ktest_expect(populated > 0, 1);

    if(populated > 0) {
        *(volatile char*)(populated + LENGTH - 1) = 1;
        ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, populated, LENGTH), 0);
    }

    // A fixed mapping without replace fails on a used range and succeeds on a free one
    long base = map_anonymous(0, 0);

    ktest_expect(base > 0, 1);

    if(base > 0) {
        ktest_expect(map_anonymous(base, __k_MAP_FIXED_NOREPLACE), -__k_EEXIST);
        ktest_expect(map_anonymous(base + PAGE_SIZE, __k_MAP_FIXED_NOREPLACE), -__k_EEXIST);
        ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, base, LENGTH), 0);
        ktest_expect(map_anonymous(base, __k_MAP_FIXED_NOREPLACE), base);
        ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, base, LENGTH), 0);
    }

    // A flag that does not exist is still refused
    ktest_expect(map_anonymous(0, 0x40000000), -__k_EINVAL);

    return ktest_result();
}
