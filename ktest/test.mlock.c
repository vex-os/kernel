#include <vex/errno.h>
#include <vex/mman.h>
#include <vex/syscall.h>

#include "ktest.h"

#define PAGE_SIZE 4096
#define PAGES     4
#define LENGTH    (PAGES * PAGE_SIZE)

static long advise(long address, long length, long advice)
{
    return ktest_syscall3(__k_SYS_MADVISE, address, length, advice);
}

static long lock(long address, long length)
{
    return ktest_syscall2(__k_SYS_MLOCK, address, length);
}

static long unlock(long address, long length)
{
    return ktest_syscall2(__k_SYS_MUNLOCK, address, length);
}

static void test_advise(long base)
{
    for(long advice = __k_MADV_NORMAL; advice <= __k_MADV_DONTNEED; ++advice) {
        ktest_expect(advise(base, LENGTH, advice), 0);
    }

    ktest_expect(advise(base, LENGTH, 99), -__k_EINVAL);
    ktest_expect(advise(base + 1, LENGTH, __k_MADV_NORMAL), -__k_EINVAL);
    ktest_expect(advise(base, 0, __k_MADV_NORMAL), 0);

    // The length is rounded up to whole pages
    ktest_expect(advise(base, 1, __k_MADV_NORMAL), 0);
    ktest_expect(advise(base, LENGTH + 1, __k_MADV_NORMAL), -__k_ENOMEM);
    ktest_expect(advise(base, LENGTH + PAGE_SIZE, __k_MADV_NORMAL), -__k_ENOMEM);
    ktest_expect(advise(0, PAGE_SIZE, __k_MADV_NORMAL), -__k_ENOMEM);
    ktest_expect(advise(base, -1, __k_MADV_NORMAL), -__k_ENOMEM);
}

static void test_lock(long base)
{
    ktest_expect(lock(base, LENGTH), 0);
    ktest_expect(lock(base, 0), 0);

    // The range is rounded out, as in Linux
    ktest_expect(lock(base + 100, 1), 0);
    ktest_expect(lock(base + PAGE_SIZE - 1, 2), 0);
    ktest_expect(unlock(base + 100, LENGTH - 200), 0);

    ktest_expect(lock(base, LENGTH + 1), -__k_ENOMEM);
    ktest_expect(lock(0, PAGE_SIZE), -__k_ENOMEM);
    ktest_expect(unlock(base, LENGTH + PAGE_SIZE), -__k_ENOMEM);
    ktest_expect(unlock(base, -1), -__k_ENOMEM);

    // A hole in the range fails the call, also where the ends are mapped
    ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, base + PAGE_SIZE, PAGE_SIZE), 0);
    ktest_expect(lock(base, LENGTH), -__k_ENOMEM);
    ktest_expect(advise(base, LENGTH, __k_MADV_NORMAL), -__k_ENOMEM);
    ktest_expect(lock(base, PAGE_SIZE), 0);
    ktest_expect(lock(base + 2 * PAGE_SIZE, 2 * PAGE_SIZE), 0);
}

static void test_lock_all(void)
{
    ktest_expect(ktest_syscall1(__k_SYS_MLOCKALL, __k_MCL_CURRENT), 0);
    ktest_expect(ktest_syscall1(__k_SYS_MLOCKALL, __k_MCL_FUTURE), 0);
    ktest_expect(ktest_syscall1(__k_SYS_MLOCKALL, __k_MCL_CURRENT | __k_MCL_FUTURE), 0);
    ktest_expect(ktest_syscall1(__k_SYS_MLOCKALL, __k_MCL_CURRENT | __k_MCL_ONFAULT), 0);

    // MCL_ONFAULT needs one of the others
    ktest_expect(ktest_syscall1(__k_SYS_MLOCKALL, __k_MCL_ONFAULT), -__k_EINVAL);
    ktest_expect(ktest_syscall1(__k_SYS_MLOCKALL, 0), -__k_EINVAL);
    ktest_expect(ktest_syscall1(__k_SYS_MLOCKALL, 8), -__k_EINVAL);

    ktest_expect(ktest_syscall1(__k_SYS_MUNLOCKALL, 0), 0);
}

int main(void)
{
    long base = ktest_syscall6(__k_SYS_MMAP, 0, LENGTH, __k_PROT_READ | __k_PROT_WRITE, __k_MAP_PRIVATE | __k_MAP_ANONYMOUS, -1, 0);

    ktest_assert(base > 0);

    test_advise(base);
    test_lock(base);
    test_lock_all();

    return ktest_result();
}
