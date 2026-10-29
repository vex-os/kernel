#include <arch/amd64.lapic.h>
#include <arch/amd64.msr.h>
#include <vex/device.h>
#include <vex/driver.h>
#include <vex/errno.h>
#include <vex/hart.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/mmio.h>
#include <vex/page.h>
#include <vex/panic.h>

#define MSR_IA32_APIC_BASE     UINT32_C(0x0000001B)
#define MSR_APIC_BASE_PHYSMASK UINT64_C(0x0000000FFFFFF000)
#define MSR_APIC_BASE_ENABLE   UINT64_C(0x0000000000000800)

#define LAPIC_SVR_ENABLE UINT32_C(0x00000100)

static volatile uint32_t* lapic_mmio = nullptr; // Shared by every hart, so no node hands it to the framework

uint32_t lapic_read(uint32_t reg)
{
    return lapic_mmio[reg];
}

void lapic_write(uint32_t reg, uint32_t value)
{
    lapic_mmio[reg] = value;
}

void lapic_ipi(uint32_t target, uint32_t mode, uint8_t vector)
{
    if(lapic_mmio) {
        bool intr_state = disable_interrupts(); // An IPI sent from an interrupt handler in between would overwrite ICR_HI

        while(lapic_read(LAPIC_REG_ICR_LO) & UINT32_C(0x00001000)) {
            hart_spin();
        }

        lapic_write(LAPIC_REG_ICR_HI, target << 24);
        lapic_write(LAPIC_REG_ICR_LO, vector | mode);

        restore_interrupts(intr_state);
    }
}

// The local APIC stays with its hart, which can't go offline yet
static void lapic_detach(device_t* restrict device)
{
    // empty
}

static void lapic_release(device_t* restrict device)
{
    // empty
}

static const driver_ops_t lapic_driver_ops = {
    .detach = &lapic_detach,
    .release = &lapic_release,
};

static const driver_match_t lapic_matches[] = {
    DRIVER_MATCH_END(),
};

// Not in the drivers' table, as the local APICs
// are brought up before the tree and never probed
static const driver_t lapic_driver = {
    .name = "lapic",
    .ops = &lapic_driver_ops,
    .matches = lapic_matches,
};

int lapic_add_device(device_t* restrict cpu, hart_local_t* restrict hart)
{
    device_t* device;
    int retval = device_add_bound(cpu, nullptr, "lapic", &lapic_driver, hart, &device);

    if(retval < 0) {
        return retval;
    }

    device_decref(device);

    return 0;
}

static void arch_init_lapic(void)
{
    uint64_t msr = rdmsr(MSR_IA32_APIC_BASE);

    if(0 == (msr & MSR_APIC_BASE_ENABLE)) {
        panic("local apic is globally disabled");
    }

    uintptr_t lapic_phys = msr & MSR_APIC_BASE_PHYSMASK;

    // Every hart maps it and gets the same address back
    lapic_mmio = mmio_map(phys_addr(lapic_phys), PAGE_SIZE, VPROT_MMIO);

    if(lapic_mmio == nullptr) {
        panic("lapic: failed to map registers");
    }

    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr |= LAPIC_SPURIOUS_VECTOR;

    lapic_write(LAPIC_REG_SVR, svr);
}

initcall_smp_irq(arch_init_lapic, INIT_FIRST);
