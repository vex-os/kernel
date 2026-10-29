#include <vex/atomic.h>
#include <vex/hart.h>
#include <vex/initcall.h>
#include <vex/irq.h>
#include <vex/limits.h>
#include <vex/panic.h>
#include <vex/pmap.h>
#include <vex/tlb.h>

static uint64_t tlb_generation;
static bool tlb_ready;
static unsigned tlb_irq_number;

static void tlb_irq_handler(unsigned number, const hart_context_t* restrict ctx, void* restrict ctxdata)
{
    tlb_poll();
}

void tlb_poll(void)
{
    if(!atomic_load_n(&tlb_ready, ATOMIC_ACQUIRE)) {
        return;
    }

    hart_local_t* hart = this_hart();

    if(!atomic_load_n(&hart->tlb_online, ATOMIC_ACQUIRE)) {
        return;
    }

    uint64_t generation = atomic_load_n(&tlb_generation, ATOMIC_SEQ_CST);

    if(atomic_load_n(&hart->tlb_flushed, ATOMIC_RELAXED) < generation) {
        pmap_flush_all();
        atomic_store_n(&hart->tlb_flushed, generation, ATOMIC_RELEASE);
    }
}

void tlb_shootdown(void)
{
    if(!atomic_load_n(&tlb_ready, ATOMIC_ACQUIRE)) {
        pmap_flush_all();
        return;
    }

    hart_local_t* self = this_hart();
    uint64_t generation = 1 + atomic_fetch_add(&tlb_generation, 1, ATOMIC_SEQ_CST);

    pmap_flush_all();
    atomic_store_n(&self->tlb_flushed, generation, ATOMIC_RELEASE);

    hart_ipi(HART_IPI_EXCEPT_ME, tlb_irq_number);

    for(unsigned long i = 0; i < num_harts; ++i) {
        hart_local_t* hart = &harts[i];

        if(hart == self || !atomic_load_n(&hart->tlb_online, ATOMIC_ACQUIRE)) {
            continue;
        }

        // hart_spin polls too, so two harts shooting down at once serve each other
        while(atomic_load_n(&hart->tlb_flushed, ATOMIC_ACQUIRE) < generation) {
            hart_spin();
        }
    }
}

void tlb_hart_online(void)
{
    hart_local_t* hart = this_hart();

    pmap_flush_all();
    uint64_t generation = atomic_load_n(&tlb_generation, ATOMIC_SEQ_CST);

    atomic_store_n(&hart->tlb_flushed, generation, ATOMIC_RELEASE);
    atomic_store_n(&hart->tlb_online, true, ATOMIC_RELEASE);
    atomic_store_n(&tlb_ready, true, ATOMIC_RELEASE);
}

static void init_tlb(void)
{
    tlb_irq_number = irq_handler_add(UINT_MAX, &tlb_irq_handler, nullptr);

    if(tlb_irq_number == UINT_MAX) {
        panic("tlb: out of irq numbers");
    }
}

initcall_bsp_mmu(init_tlb, INIT_MIDDLE);
