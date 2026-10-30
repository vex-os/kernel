#include <arch/irq.h>
#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/hart.h>
#include <vex/irq.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/limits.h>
#include <vex/random_source.h>
#include <vex/spinlock.h>
#include <vex/string.h>

#define IRQ_TABLE_SIZE (ARCH_IRQ_MAX - ARCH_IRQ_BASE + 1)

typedef struct irq_entry_s irq_entry_t;

struct irq_entry_s {
    irq_handler_t handler;
    void* ctxdata;
    unsigned in_flight;
};

static spinlock_t irq_table_lock = SPINLOCK_INIT;
static irq_entry_t irq_table[IRQ_TABLE_SIZE] = {}; // Numbers that are the same on all harts
static unsigned hart_users[IRQ_TABLE_SIZE] = {};   // The number of harts that use the number for their own handler
static irq_entry_t* hart_tables[MAX_HARTS] = {};   // Allocated with the first handler of the hart. Never freed

// The timing of interrupts under a hypervisor
// can be close to regular, so it earns little
define_random_source_event(irq, true, 64);

static void claim_nolock(irq_entry_t* restrict entry, irq_handler_t handler, void* restrict ctxdata)
{
    entry->ctxdata = ctxdata;

    atomic_store(&entry->handler, handler, ATOMIC_RELEASE);
}

static void release_nolock(irq_entry_t* restrict entry)
{
    atomic_store(&entry->handler, nullptr, ATOMIC_SEQ_CST);

    while(atomic_load(&entry->in_flight, ATOMIC_SEQ_CST)) {
        hart_spin();
    }

    entry->ctxdata = nullptr;
}

static bool is_free_everywhere(unsigned number)
{
    return irq_table[number].handler == nullptr && hart_users[number] == 0;
}

static irq_entry_t* table_of(hart_local_t* restrict hart)
{
    irq_entry_t* table = atomic_load(&hart_tables[hart->index], ATOMIC_ACQUIRE);

    if(table) {
        return table;
    }

    irq_entry_t* allocated = kmalloc(IRQ_TABLE_SIZE * sizeof(irq_entry_t));

    if(allocated == nullptr) {
        return nullptr;
    }

    memset(allocated, 0, IRQ_TABLE_SIZE * sizeof(irq_entry_t));

    bool intr_state = spinlock_acquire_intr(&irq_table_lock);

    table = hart_tables[hart->index];

    if(table == nullptr) {
        table = allocated;
        allocated = nullptr;
        atomic_store(&hart_tables[hart->index], table, ATOMIC_RELEASE);
    }

    spinlock_release_intr(&irq_table_lock, intr_state);

    kfree(allocated);

    return table;
}

// Before the harts are set up, only the boot hart gets interrupts
static irq_entry_t* entry_of(unsigned number)
{
    irq_entry_t* entry = &irq_table[number];

    if(atomic_load(&entry->handler, ATOMIC_SEQ_CST)) {
        return entry;
    }

    hart_local_t* hart = this_hart();

    if(hart == nullptr) {
        hart = boot_hart();
    }

    irq_entry_t* table = atomic_load(&hart_tables[hart->index], ATOMIC_ACQUIRE);

    if(table == nullptr) {
        return nullptr;
    }

    return &table[number];
}

unsigned irq_handler_add(unsigned desired_number, irq_handler_t handler, void* restrict ctxdata)
{
    bool intr_state = spinlock_acquire_intr(&irq_table_lock);

    unsigned number = desired_number;

    if(number >= IRQ_TABLE_SIZE || !is_free_everywhere(number)) {
        number = UINT_MAX;

        for(unsigned i = IRQ_TABLE_SIZE; i > 0; --i) {
            if(is_free_everywhere(i - 1)) {
                number = i - 1;
                break;
            }
        }
    }

    if(number != UINT_MAX) {
        claim_nolock(&irq_table[number], handler, ctxdata);
    }

    spinlock_release_intr(&irq_table_lock, intr_state);

    return number;
}

void irq_handler_remove(unsigned number)
{
    bool intr_state = spinlock_acquire_intr(&irq_table_lock);

    release_nolock(&irq_table[number]);

    spinlock_release_intr(&irq_table_lock, intr_state);
}

// Numbers for all harts come from the top, because the CPU gives higher vectors a higher priority, and IPIs
// and timers must be above devices. Numbers for one hart come from the bottom. Each kind keeps space for the other
unsigned irq_handler_add_hart(hart_local_t* restrict hart, irq_handler_t handler, void* restrict ctxdata)
{
    irq_entry_t* table = table_of(hart);

    if(table == nullptr) {
        return UINT_MAX;
    }

    bool intr_state = spinlock_acquire_intr(&irq_table_lock);

    for(unsigned number = 0; number < IRQ_TABLE_SIZE; number += 1) {
        if(irq_table[number].handler == nullptr && table[number].handler == nullptr) {
            claim_nolock(&table[number], handler, ctxdata);
            hart_users[number] += 1;
            spinlock_release_intr(&irq_table_lock, intr_state);
            return number;
        }
    }

    spinlock_release_intr(&irq_table_lock, intr_state);

    return UINT_MAX;
}

void irq_handler_remove_hart(hart_local_t* restrict hart, unsigned number)
{
    irq_entry_t* table = hart_tables[hart->index];

    kassert(table && table[number].handler);

    bool intr_state = spinlock_acquire_intr(&irq_table_lock);

    release_nolock(&table[number]);
    hart_users[number] -= 1;

    spinlock_release_intr(&irq_table_lock, intr_state);
}

void irq_dispatch(unsigned number, const hart_context_t* restrict ctx)
{
    random_add_event(irq, number);

    if(number >= IRQ_TABLE_SIZE) {
        pr_verbose("irq: spurious IRQ 0x%02X out of range", number);
        return;
    }

    irq_entry_t* entry = entry_of(number);
    irq_handler_t handler = nullptr;

    if(entry) {
        atomic_fetch_add(&entry->in_flight, 1, ATOMIC_SEQ_CST);

        handler = atomic_load(&entry->handler, ATOMIC_SEQ_CST);

        if(handler) {
            handler(number, ctx, entry->ctxdata);
        }

        atomic_fetch_sub(&entry->in_flight, 1, ATOMIC_RELEASE);
    }

    if(handler == nullptr) {
        pr_verbose("irq: unhandled IRQ 0x%02X", number);
    }
}
