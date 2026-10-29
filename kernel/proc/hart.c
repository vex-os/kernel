#include <vex/atomic.h>
#include <vex/cpucaps.h>
#include <vex/hart.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/panic.h>
#include <vex/pmap.h>
#include <vex/sched.h>
#include <vex/thread.h>
#include <vex/tlb.h>

hart_local_t harts[MAX_HARTS];
unsigned long num_harts = 1U;

static unsigned long ignored_harts = 0U;
static unsigned long usable_harts = 1U;
static volatile unsigned long probed_harts = 0U;
static volatile unsigned long ready_harts = 0U;

static void bring_online(hart_local_t* restrict hart)
{
    hart_setup(hart);

    invoke_initcalls(initcalls_smp_start, initcalls_smp_end);

    tlb_hart_online();

    hart->flags |= HART_FLAG_ONLINE;

    atomic_fetch_add(&ready_harts, 1, ATOMIC_RELEASE);
}

static void __noreturn run_idle(hart_local_t* restrict hart)
{
    pmap_switch(&sys_pmap);

    bring_online(hart);

    enable_interrupts();

    for(;;) {
        hart_stop();
    }
}

hart_local_t* this_hart(void)
{
    thread_t* thread = this_thread();

    return thread->last_hart;
}

bool hart_online(unsigned long index)
{
    hart_local_t* hart = &harts[index];

    return atomic_load_n(&hart->flags, ATOMIC_RELAXED) & HART_FLAG_ONLINE;
}

hart_local_t* hart_register(bool is_boot)
{
    hart_local_t* hart;

    if(is_boot) {
        hart = &harts[0];
    }
    else if(num_harts < MAX_HARTS) {
        hart = &harts[num_harts];
        num_harts += 1;
    }
    else {
        ignored_harts += 1;
        return nullptr;
    }

    hart->index = (unsigned short)(hart - harts);

    hart->flags = HART_FLAG_PRESENT | HART_FLAG_ACTIVE;

    if(is_boot) {
        hart->flags |= HART_FLAG_BSP;
    }

    return hart;
}

void hart_park(hart_local_t* restrict hart)
{
    arch_cpucaps_probe(&hart->caps);

    atomic_fetch_add(&probed_harts, 1, ATOMIC_RELEASE);

    unsigned char bringup;

    while(HART_BRINGUP_WAIT == (bringup = atomic_load_n(&hart->bringup, ATOMIC_ACQUIRE))) {
        hart_relax();
    }

    if(bringup == HART_BRINGUP_GO) {
        run_idle(hart);
    }

    for(;;) {
        disable_interrupts();
        hart_stop();
    }
}

static void init_harts(void)
{
    arch_enumerate_harts();

    if(0 == (harts[0].flags & HART_FLAG_PRESENT)) {
        panic("hart: firmware did not report the boot hart");
    }

    if(ignored_harts) {
#ifdef CONFIG_SMP
        pr_warning("hart: %lu harts past CONFIG_MAX_HARTS=%u left unused", ignored_harts, MAX_HARTS);
#else
        pr_inform("hart: SMP is disabled, %lu harts left unused", ignored_harts);
#endif
    }

    hart_local_t* bsp = boot_hart();

    arch_cpucaps_probe(&bsp->caps);

    for(unsigned long i = 1U; i < num_harts; ++i) {
        arch_start_hart(&harts[i]);
    }

    while(atomic_load_n(&probed_harts, ATOMIC_ACQUIRE) < num_harts - 1U) {
        hart_relax();
    }

    cpucaps_init(&bsp->caps);

    for(unsigned long i = 1U; i < num_harts; ++i) {
        if(cpucaps_merge(&harts[i].caps, harts[i].index)) {
            usable_harts += 1U;
        }
        else {
            atomic_store_n(&harts[i].bringup, HART_BRINGUP_PARK, ATOMIC_RELEASE);
        }
    }

    cpucaps_commit();
}

static void init_hart_boot(void)
{
    bring_online(boot_hart());

    for(unsigned long i = 1U; i < num_harts; ++i) {
        if(harts[i].bringup == HART_BRINGUP_WAIT) {
            atomic_store_n(&harts[i].bringup, HART_BRINGUP_GO, ATOMIC_RELEASE);
        }
    }

    while(atomic_load_n(&ready_harts, ATOMIC_ACQUIRE) < usable_harts) {
        hart_spin();
    }

    pr_verbose("%lu of %lu harts online", usable_harts, num_harts);
}

initcall_bsp_arch(init_harts, INIT_FIRST);
initcall_bsp_multicore(init_hart_boot, INIT_LAST);
