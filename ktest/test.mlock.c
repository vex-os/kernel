#include <vex/errno.h>
#include <vex/fcntl.h>
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

static bool page_filled(long page, char value)
{
    const volatile char* bytes = (const volatile char*)(page);

    for(long i = 0; i < PAGE_SIZE; ++i) {
        if(bytes[i] != value) {
            return false;
        }
    }

    return true;
}

static void fill_page(long page, char value)
{
    volatile char* bytes = (volatile char*)(page);

    for(long i = 0; i < PAGE_SIZE; ++i) {
        bytes[i] = value;
    }
}

static void test_dontneed_anonymous(long base)
{
    for(long page = 0; page < PAGES; ++page) {
        fill_page(base + page * PAGE_SIZE, 'a' + page);
    }

    // The pages of the middle go, the regions stay, and the next access gets zeros
    ktest_expect(advise(base + PAGE_SIZE, 2 * PAGE_SIZE, __k_MADV_DONTNEED), 0);
    ktest_expect(page_filled(base, 'a'), 1);
    ktest_expect(page_filled(base + PAGE_SIZE, 0), 1);
    ktest_expect(page_filled(base + 2 * PAGE_SIZE, 0), 1);
    ktest_expect(page_filled(base + 3 * PAGE_SIZE, 'd'), 1);

    // The pages are writable again, with no new mapping
    fill_page(base + PAGE_SIZE, 'x');
    ktest_expect(page_filled(base + PAGE_SIZE, 'x'), 1);

    // A part of a page counts as the whole page
    ktest_expect(advise(base + 3 * PAGE_SIZE, 1, __k_MADV_DONTNEED), 0);
    ktest_expect(page_filled(base + 3 * PAGE_SIZE, 0), 1);
}

static void test_dontneed_file(void)
{
    long fd = ktest_open("madvise.bin", __k_O_RDWR | __k_O_CREAT, 0644);
    char content[PAGE_SIZE];

    ktest_assert(fd >= 0);

    for(long i = 0; i < PAGE_SIZE; ++i) {
        content[i] = 'f';
    }

    ktest_expect(ktest_write(fd, content, PAGE_SIZE), PAGE_SIZE);

    long privately = ktest_syscall6(__k_SYS_MMAP, 0, PAGE_SIZE, __k_PROT_READ | __k_PROT_WRITE, __k_MAP_PRIVATE, fd, 0);
    long shared = ktest_syscall6(__k_SYS_MMAP, 0, PAGE_SIZE, __k_PROT_READ | __k_PROT_WRITE, __k_MAP_SHARED, fd, 0);

    ktest_assert(privately > 0 && shared > 0);

    // The copy that a private write made goes, and the file shows through again
    fill_page(privately, 'p');
    ktest_expect(page_filled(privately, 'p'), 1);
    ktest_expect(advise(privately, PAGE_SIZE, __k_MADV_DONTNEED), 0);
    ktest_expect(page_filled(privately, 'f'), 1);

    // A shared page keeps its data, which is in the file and not in the mapping
    fill_page(shared, 's');
    ktest_expect(advise(shared, PAGE_SIZE, __k_MADV_DONTNEED), 0);
    ktest_expect(page_filled(shared, 's'), 1);

    ktest_close(fd);
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
    test_dontneed_anonymous(base);
    test_dontneed_file();
    test_lock(base);
    test_lock_all();

    return ktest_result();
}
