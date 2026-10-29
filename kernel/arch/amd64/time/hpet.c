#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <vex/clocksrc.h>
#include <vex/device.h>
#include <vex/driver.h>
#include <vex/errno.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/mmio.h>
#include <vex/panic.h>
#include <vex/vprot.h>

#define HPET_REG_CAPS   0x000
#define HPET_REG_CONFIG 0x010
#define HPET_REG_MAIN   0x0F0

#define HPET_MMIO_SIZE 0x400

#define HPET_CAPS_PERIOD_SHIFT UINT64_C(32)
#define HPET_CAPS_COUNT_64BIT  UINT64_C(0x2000)
#define HPET_CONFIG_ENABLE     UINT64_C(1)
#define HPET_PERIOD_MAX        UINT64_C(100000000)

static inline uint64_t hpet_read_reg(volatile uint64_t* restrict mmio, uint32_t offset)
{
    return mmio[offset / sizeof(uint64_t)];
}

static inline void hpet_write_reg(volatile uint64_t* restrict mmio, uint32_t offset, uint64_t value)
{
    mmio[offset / sizeof(uint64_t)] = value;
}

static uint64_t hpet_read(clock_source_t* restrict source)
{
    volatile uint64_t* mmio = source->ctxdata;

    return hpet_read_reg(mmio, HPET_REG_MAIN);
}

static const clock_source_ops_t hpet_ops = {
    .read = &hpet_read,
};

static const driver_match_t hpet_matches[] = {
    DRIVER_MATCH_FIRMWARE("PNP0103"),
    DRIVER_MATCH_END(),
};

static clock_source_t hpet_clock_source = {
    .ops = &hpet_ops,
    .name = "hpet",
    .rating = 200,
};

static phys_addr_t hpet_address = {}; // Where the counting HPET is, or null while none counts
static uint64_t hpet_frequency = 0;

static int hpet_probe(device_t* restrict device, const driver_match_t* restrict match)
{
    return DRIVER_SCORE_DEFAULT;
}

// The node of the HPET that counts since before the tree, which takes over its
// registers; another HPET, or one the timer stage gave up on, stays unbound
static int hpet_attach(device_t* restrict device, const driver_match_t* restrict match)
{
    uint64_t address;

    if(address_null(hpet_address) || device_get_io_address(device, 0, &address) < 0 || address != hpet_address.value) {
        return -ENODEV;
    }

    io_region_t region = {};
    region.space = IO_SPACE_MEMORY;
    region.base = (uintptr_t)(hpet_clock_source.ctxdata);
    region.length = HPET_MMIO_SIZE;

    int retval = device_adopt_io(device, &region);

    if(retval < 0) {
        return retval;
    }

    device->ctxdata = &hpet_clock_source;

    return 0;
}

// UNDONE: switch to another clock source first
// once removing the active one keeps the time going
// Time goes on from another clock source, so the counter can go
static int hpet_offline(device_t* restrict device)
{
    if(clock_source_remove(&hpet_clock_source) == -EBUSY) {
        pr_warning("hpet: %s is the only clock source", device->name);
        return -EBUSY;
    }

    return 0;
}

static void hpet_online(device_t* restrict device)
{
    clock_source_add_raw(&hpet_clock_source, UINT64_C(1), hpet_frequency);
}

// A counter that stopped still beats time going back to zero, so the only source stays
static void hpet_detach(device_t* restrict device)
{
    if(device_is_gone(device) && clock_source_remove(&hpet_clock_source) == -EBUSY) {
        pr_warning("hpet: %s is gone, but time has nothing else to go on", device->name);
    }
}

// The framework has unmapped the registers by now
static void hpet_release(device_t* restrict device)
{
    if(device->ctxdata == nullptr) {
        return;
    }

    hpet_clock_source.ctxdata = nullptr;
    hpet_address = phys_addr(0);
}

static const driver_ops_t hpet_driver_ops = {
    .probe = &hpet_probe,
    .attach = &hpet_attach,
    .offline = &hpet_offline,
    .online = &hpet_online,
    .detach = &hpet_detach,
    .release = &hpet_release,
};

define_driver(hpet, &hpet_driver_ops, hpet_matches);

static void arch_init_hpet(void)
{
    uacpi_table table;
    uacpi_status status = uacpi_table_find_by_signature(ACPI_HPET_SIGNATURE, &table);

    if(uacpi_likely_success(status)) {
        struct acpi_hpet* hpet = (struct acpi_hpet*)(table.hdr);

        if(hpet->address.address) {
            phys_addr_t address = phys_addr(hpet->address.address);
            volatile uint64_t* mmio = mmio_map(address, HPET_MMIO_SIZE, VPROT_MMIO);

            if(mmio == nullptr) {
                panic("hpet: failed to map registers");
            }

            uacpi_table_unref(&table);

            uint64_t hpet_caps = hpet_read_reg(mmio, HPET_REG_CAPS);
            uint64_t hpet_period_fsec = hpet_caps >> HPET_CAPS_PERIOD_SHIFT;

            if(hpet_period_fsec == 0 || hpet_period_fsec > HPET_PERIOD_MAX) {
                pr_verbose("hpet: invalid counter period %llu fs", (unsigned long long)(hpet_period_fsec));
                mmio_unmap(mmio);
                return;
            }

            hpet_clock_source.ctxdata = (void*)(mmio); // hpet_read_reg adds volatile back
            hpet_address = address;

            uint32_t hpet_config_reg = hpet_read_reg(mmio, HPET_REG_CONFIG);
            hpet_config_reg |= HPET_CONFIG_ENABLE;

            hpet_write_reg(mmio, HPET_REG_CONFIG, hpet_config_reg);

            if(hpet_caps & HPET_CAPS_COUNT_64BIT) {
                hpet_clock_source.mask = CLOCK_SOURCE_MASK_64;
            }
            else {
                hpet_clock_source.mask = CLOCK_SOURCE_MASK_32;
            }

            hpet_frequency = UINT64_C(1000000000000000) / hpet_period_fsec;
            clock_source_add_raw(&hpet_clock_source, UINT64_C(1), hpet_frequency);
        }
    }
}

initcall_bsp_timer(arch_init_hpet, INIT_FIRST);
