#include <limine.h>
#include <vex/errno.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/random_source.h>
#include <vex/string.h>
#include <vex/strings.h>

#define ENTROPY_VALUES 8
#define ENTROPY_BYTES  (ENTROPY_VALUES * sizeof(uint64_t))
#define ENTROPY_BITS   (ENTROPY_BYTES * 8)

static_assert(ENTROPY_BYTES <= RANDOM_SOURCE_SAMPLE_MAX);

static volatile __used __section(".limine_requests") struct limine_entropy_request entropy_request = {
    .id = LIMINE_ENTROPY_REQUEST_ID,
    .revision = 0,
    .value_count = ENTROPY_VALUES,
};

// The response lives in bootloader memory, which
// the kernel reclaims, so the values move here first
static uint8_t values[ENTROPY_BYTES];
static size_t value_bytes;

static bool limine_probe(void)
{
    return value_bytes > 0;
}

// Delivers the values once
static ssize_t limine_poll(void* out, size_t size)
{
    if(value_bytes == 0) {
        return -ENODEV;
    }

    size_t bytes = value_bytes;

    memcpy(out, values, bytes);
    memzero_explicit(values, sizeof(values));

    value_bytes = 0;

    return bytes;
}

// Limine takes the values from RDSEED, RDRAND and the UEFI RNG and falls
// back to PCG32, and its response does not say which one made a value. So
// the bootloader is trusted as a whole, unless the command line says otherwise
define_random_source(limine, true, ENTROPY_BYTES, ENTROPY_BITS, limine_probe, limine_poll);

static void init_limine_entropy(void)
{
    const struct limine_entropy_response* response = entropy_request.response;

    if(response == nullptr) {
        pr_warning("random: the bootloader gave no entropy");
        return;
    }

    size_t count = response->value_count;

    if(count > ENTROPY_VALUES) {
        count = ENTROPY_VALUES;
    }

    value_bytes = count * sizeof(uint64_t);

    memcpy(values, response->values, value_bytes);
    memzero_explicit(response->values, value_bytes);
}

initcall_bsp_bootinfo(init_limine_entropy, INIT_LATE);
