#include <limine.h>
#include <vex/cmdline.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/random.h>
#include <vex/string.h>
#include <vex/strings.h>

#define ENTROPY_VALUES 8

static volatile __used __section(".limine_requests") struct limine_entropy_request entropy_request = {
    .id = LIMINE_ENTROPY_REQUEST_ID,
    .revision = 0,
    .value_count = ENTROPY_VALUES,
};

// Limine takes the values from RDSEED, RDRAND and the UEFI RNG and falls back to PCG32,
// and its response does not say which one made a value. So the bootloader is trusted
// as a whole, unless the command line says otherwise
static bool bootloader_trusted(void)
{
    const char* argument = cmdline_argument(&sys_cmdline, "random.trust_bootloader");

    if(argument && 0 == strcmp(argument, "0")) {
        return false;
    }

    return true;
}

// Before the bootloader memory is reclaimed
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

    size_t size = count * sizeof(uint64_t);
    unsigned credit_bits = 0;

    if(bootloader_trusted()) {
        credit_bits = size * 8;
    }

    random_add(response->values, size, credit_bits);

    memzero_explicit(response->values, size);

    pr_inform("random: %zu values from the bootloader, %u bits credited", count, credit_bits);
}

initcall_bsp_bootinfo(init_limine_entropy, INIT_LATE);
