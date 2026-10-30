#include <vex/errno.h>
#include <vex/mman.h>
#include <vex/random.h>
#include <vex/syscall.h>

#include "ktest.h"

#define PAGE_SIZE   4096
#define CANARY      0xA5
#define CANARY_SIZE 16
#define BIG_SIZE    (64 * PAGE_SIZE)

#define BALANCE_SIZE  4096
#define BALANCE_BITS  (BALANCE_SIZE * 8)
#define BALANCE_SLACK 500

static long get_random(void* buffer, long size, long flags)
{
    return ktest_syscall3(__k_SYS_GETRANDOM, (long)(buffer), size, flags);
}

static bool is_zero(const unsigned char* bytes, size_t size)
{
    for(size_t i = 0; i < size; ++i) {
        if(bytes[i]) {
            return false;
        }
    }

    return true;
}

static void test_flags(void)
{
    unsigned char buffer[8];

    ktest_expect(get_random(buffer, sizeof(buffer), 0x8), -__k_EINVAL);
    ktest_expect(get_random(buffer, sizeof(buffer), 0x4), -__k_EINVAL);
    ktest_expect(get_random(buffer, sizeof(buffer), -1), -__k_EINVAL);

    ktest_expect(get_random(buffer, sizeof(buffer), __k_GRND_NONBLOCK), sizeof(buffer));
    ktest_expect(get_random(buffer, sizeof(buffer), __k_GRND_INSECURE), sizeof(buffer));
    ktest_expect(get_random(buffer, sizeof(buffer), __k_GRND_NONBLOCK | __k_GRND_INSECURE), sizeof(buffer));
}

static void test_bad_buffer(void)
{
    ktest_expect(get_random(nullptr, 4, 0), -__k_EFAULT);
    ktest_expect(get_random(nullptr, 4, __k_GRND_INSECURE), -__k_EFAULT);
    ktest_expect(get_random(nullptr, 0, 0), 0);
}

// The bytes after the requested ones stay untouched, and the call never reports more than it wrote
static void test_lengths(void)
{
    static const long lengths[] = { 0, 1, 2, 31, 32, 33, 63, 64, 65, 255, 256, 257, 511, 512, 513, 1000, 4096, 5000 };

    for(size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        long length = lengths[i];
        unsigned char buffer[5000 + CANARY_SIZE];

        for(size_t j = 0; j < sizeof(buffer); ++j) {
            buffer[j] = CANARY;
        }

        ktest_expect(get_random(buffer, length, 0), length);

        for(long j = length; j < length + CANARY_SIZE; ++j) {
            ktest_expect(buffer[j], CANARY);
        }

        // A run of 16 zero bytes has a chance of 2^-128
        if(length >= 16) {
            ktest_expect(is_zero(buffer, length), 0);
        }
    }
}

static void test_distinct(void)
{
    unsigned char first[32];
    unsigned char second[32];

    ktest_expect(get_random(first, sizeof(first), 0), sizeof(first));
    ktest_expect(get_random(second, sizeof(second), 0), sizeof(second));
    ktest_expect(ktest_bytes_equal(first, second, sizeof(first)), 0);
}

// Far from a proof of randomness: it only catches a stream with a stuck or a missing half of the bits
static void test_balance(void)
{
    unsigned char buffer[BALANCE_SIZE];
    long ones = 0;

    ktest_expect(get_random(buffer, sizeof(buffer), 0), sizeof(buffer));

    for(size_t i = 0; i < sizeof(buffer); ++i) {
        for(unsigned bit = 0; bit < 8; bit += 1) {
            ones += (buffer[i] >> bit) & 1;
        }
    }

    ktest_expect(ones > BALANCE_BITS / 2 - BALANCE_SLACK, 1);
    ktest_expect(ones < BALANCE_BITS / 2 + BALANCE_SLACK, 1);
}

static void test_big(void)
{
    long base = ktest_syscall6(__k_SYS_MMAP, 0, BIG_SIZE, __k_PROT_READ | __k_PROT_WRITE, __k_MAP_PRIVATE | __k_MAP_ANONYMOUS, -1, 0);

    ktest_expect(base > 0, 1);

    if(base <= 0) {
        return;
    }

    ktest_expect(get_random((void*)(base), BIG_SIZE, 0), BIG_SIZE);

    // Every page holds something, so that a chunk that went missing shows
    for(long page = 0; page < BIG_SIZE / PAGE_SIZE; page += 1) {
        ktest_expect(is_zero((const unsigned char*)(base + page * PAGE_SIZE), PAGE_SIZE), 0);
    }

    ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, base, BIG_SIZE), 0);
}

// A fault after some bytes ends the call with those bytes, and a fault at the start is an error
static void test_fault_after_start(void)
{
    long base = ktest_syscall6(__k_SYS_MMAP, 0, 2 * PAGE_SIZE, __k_PROT_READ | __k_PROT_WRITE, __k_MAP_PRIVATE | __k_MAP_ANONYMOUS, -1, 0);

    ktest_expect(base > 0, 1);

    if(base <= 0) {
        return;
    }

    ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, base + PAGE_SIZE, PAGE_SIZE), 0);

    long room = 300;
    long result = get_random((void*)(base + PAGE_SIZE - room), 1000, 0);

    ktest_expect(result > 0, 1);
    ktest_expect(result <= room, 1);

    ktest_expect(get_random((void*)(base + PAGE_SIZE), 16, 0), -__k_EFAULT);

    ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, base, PAGE_SIZE), 0);
}

int main(void)
{
    test_flags();
    test_bad_buffer();
    test_lengths();
    test_distinct();
    test_balance();
    test_big();
    test_fault_after_start();

    return ktest_result();
}
