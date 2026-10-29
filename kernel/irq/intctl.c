#include <arch/irq.h>
#include <arch/msi.h>
#include <vex/assert.h>
#include <vex/errno.h>
#include <vex/hart.h>
#include <vex/intctl.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/limits.h>
#include <vex/mutex.h>
#include <vex/spinlock.h>

typedef struct interrupt_line_s interrupt_line_t;

// A GSI or a message-signalled source, routed to one vector of one hart, with the handlers that share it
struct interrupt_line_s {
    interrupt_line_t* next;             // Among the GSI lines
    interrupt_controller_t* controller; // nullptr for a message-signalled source
    const interrupt_message_ops_t* message_ops;
    void* source; // The argument for each call to message_ops
    uint32_t gsi;
    unsigned trigger;
    unsigned polarity;
    unsigned flags;
    hart_local_t* hart;
    unsigned irq_number; // Valid on hart only
    spinlock_t lock;     // Protects actions. Held while they run
    interrupt_action_t* actions;
};

struct interrupt_action_s {
    interrupt_action_t* next;
    interrupt_line_t* line;
    irq_handler_t handler;
    void* ctxdata;
};

static spinlock_t interrupt_controller_lock = SPINLOCK_INIT;
static interrupt_controller_t* interrupt_controller_head = nullptr;
static mutex_t line_mutex = MUTEX_INIT;
static interrupt_line_t* line_head = nullptr;
static unsigned hart_lines[MAX_HARTS] = {}; // Under line_mutex

static void dispatch_line(unsigned number, const hart_context_t* restrict ctx, void* restrict ctxdata)
{
    interrupt_line_t* line = ctxdata;

    spinlock_acquire(&line->lock);

    for(interrupt_action_t* action = line->actions; action; action = action->next) {
        action->handler(number, ctx, action->ctxdata);
    }

    spinlock_release(&line->lock);
}

static interrupt_line_t* find_line(uint32_t gsi)
{
    for(interrupt_line_t* line = line_head; line; line = line->next) {
        if(line->gsi == gsi) {
            return line;
        }
    }

    return nullptr;
}

static bool may_join(const interrupt_line_t* restrict line, unsigned trigger, unsigned polarity, unsigned flags)
{
    return (line->flags & flags & INTERRUPT_SHARED) && line->trigger == trigger && line->polarity == polarity;
}

static bool try_hart(interrupt_line_t* restrict line, hart_local_t* restrict hart)
{
    unsigned irq_number = irq_handler_add_hart(hart, &dispatch_line, line);

    if(irq_number == UINT_MAX) {
        return false;
    }

    line->hart = hart;
    line->irq_number = irq_number;
    hart_lines[hart->index] += 1;

    return true;
}

// The online hart with the fewest lines gets the next line
// Before the harts come up, this is the boot hart
static int bind_line(interrupt_line_t* restrict line)
{
    hart_local_t* best = boot_hart();

    for(unsigned long i = 1; i < num_harts; ++i) {
        hart_local_t* hart = &harts[i];

        if((hart->flags & HART_FLAG_ONLINE) && hart_lines[i] < hart_lines[best->index]) {
            best = hart;
        }
    }

    if(try_hart(line, best)) {
        return 0;
    }

    for(unsigned long i = 0; i < num_harts; ++i) {
        hart_local_t* hart = &harts[i];

        bool eligible = hart != best && (i == 0 || (hart->flags & HART_FLAG_ONLINE));

        if(eligible && try_hart(line, hart)) {
            return 0;
        }
    }

    return -ENOMEM;
}

static void unbind_line(interrupt_line_t* restrict line)
{
    irq_handler_remove_hart(line->hart, line->irq_number);
    hart_lines[line->hart->index] -= 1;
}

// The source must be masked
static void program_line(interrupt_line_t* restrict line)
{
    uint8_t vector = line->irq_number + ARCH_IRQ_BASE;

    if(line->controller) {
        interrupt_controller_route(line->controller, line->gsi, vector, line->hart, line->trigger, line->polarity);
        return;
    }

    uint64_t address;
    uint32_t data;
    arch_msi_compose(line->hart, vector, &address, &data);

    line->message_ops->write(line->source, address, data);
}

static void mask_line(interrupt_line_t* restrict line)
{
    if(line->controller) {
        interrupt_controller_mask(line->controller, line->gsi);
    }
    else {
        line->message_ops->mask(line->source);
    }
}

static void unmask_line(interrupt_line_t* restrict line)
{
    if(line->controller) {
        interrupt_controller_unmask(line->controller, line->gsi);
    }
    else {
        line->message_ops->unmask(line->source);
    }
}

static interrupt_line_t* alloc_line(void)
{
    interrupt_line_t* line = kmalloc(sizeof(interrupt_line_t));

    if(line == nullptr) {
        return nullptr;
    }

    *line = (interrupt_line_t) {};
    line->lock = SPINLOCK_INIT;

    if(bind_line(line) < 0) {
        kfree(line);
        return nullptr;
    }

    return line;
}

static int create_line(uint32_t gsi, unsigned trigger, unsigned polarity, unsigned flags, interrupt_line_t** restrict created)
{
    interrupt_controller_t* controller = interrupt_controller_find(gsi);

    if(controller == nullptr) {
        return -ENODEV;
    }

    interrupt_line_t* line = alloc_line();

    if(line == nullptr) {
        return -ENOMEM;
    }

    line->controller = controller;
    line->gsi = gsi;
    line->trigger = trigger;
    line->polarity = polarity;
    line->flags = flags;

    program_line(line);

    pr_verbose("intctl: GSI %u on hart %hu, vector 0x%02X", gsi, line->hart->index, line->irq_number + ARCH_IRQ_BASE);

    line->next = line_head;
    line_head = line;

    *created = line;

    return 0;
}

static void destroy_line(interrupt_line_t* restrict line)
{
    mask_line(line);
    unbind_line(line);

    if(line->controller) {
        interrupt_line_t** prev = &line_head;

        while(*prev != line) {
            prev = &(*prev)->next;
        }

        *prev = line->next;
    }

    kfree(line);
}

static void add_action(interrupt_line_t* restrict line, interrupt_action_t* restrict action)
{
    action->line = line;

    bool intr_state = spinlock_acquire_intr(&line->lock);
    action->next = line->actions;
    line->actions = action;
    spinlock_release_intr(&line->lock, intr_state);

    unmask_line(line);
}

int interrupt_controller_add(interrupt_controller_t* restrict controller)
{
    spinlock_acquire(&interrupt_controller_lock);

    for(interrupt_controller_t* it = interrupt_controller_head; it; it = it->priv.next) {
        if(it == controller) {
            spinlock_release(&interrupt_controller_lock);
            return -EEXIST;
        }
    }

    controller->priv.next = interrupt_controller_head;
    interrupt_controller_head = controller;

    spinlock_release(&interrupt_controller_lock);

    size_t gsi_base = controller->gsi_base;
    size_t gsi_count = controller->gsi_count;
    pr_verbose("intctl: adding %s: gsi_base=%zu, gsi_count=%zu", controller->name, gsi_base, gsi_count);

    return 0;
}

void interrupt_controller_remove(interrupt_controller_t* restrict controller)
{
    mutex_acquire(&line_mutex);

    for(interrupt_line_t* line = line_head; line; line = line->next) {
        kassert(line->controller != controller);
    }

    mutex_release(&line_mutex);

    spinlock_acquire(&interrupt_controller_lock);

    interrupt_controller_t** prev = &interrupt_controller_head;

    while(*prev != controller) {
        prev = &(*prev)->priv.next;
    }

    *prev = controller->priv.next;

    spinlock_release(&interrupt_controller_lock);

    pr_verbose("intctl: removed %s", controller->name);
}

interrupt_controller_t* interrupt_controller_find(uint32_t gsi)
{
    spinlock_acquire(&interrupt_controller_lock);

    for(interrupt_controller_t* it = interrupt_controller_head; it; it = it->priv.next) {
        if(gsi >= it->gsi_base && gsi < (it->gsi_base + it->gsi_count)) {
            spinlock_release(&interrupt_controller_lock);
            return it;
        }
    }

    spinlock_release(&interrupt_controller_lock);

    return nullptr;
}

void interrupt_controller_mask(interrupt_controller_t* restrict controller, uint32_t gsi)
{
    controller->ops->mask(controller, gsi);
}

void interrupt_controller_unmask(interrupt_controller_t* restrict controller, uint32_t gsi)
{
    controller->ops->unmask(controller, gsi);
}

void interrupt_controller_route(interrupt_controller_t* restrict controller, uint32_t gsi, uint8_t vector,
    const hart_local_t* restrict hart, unsigned trigger, unsigned polarity)
{
    controller->ops->route(controller, gsi, vector, hart, trigger, polarity);
}

int interrupt_attach(uint32_t gsi, unsigned trigger, unsigned polarity, unsigned flags, irq_handler_t handler, void* restrict ctxdata,
    interrupt_action_t** restrict action)
{
    interrupt_action_t* attached = kmalloc(sizeof(interrupt_action_t));

    if(attached == nullptr) {
        return -ENOMEM;
    }

    attached->handler = handler;
    attached->ctxdata = ctxdata;

    mutex_acquire(&line_mutex);

    interrupt_line_t* line = find_line(gsi);

    if(line && !may_join(line, trigger, polarity, flags)) {
        mutex_release(&line_mutex);
        kfree(attached);
        return -EBUSY;
    }

    int retval = 0;

    if(line == nullptr) {
        retval = create_line(gsi, trigger, polarity, flags, &line);
    }

    if(retval < 0) {
        mutex_release(&line_mutex);
        kfree(attached);
        return retval;
    }

    add_action(line, attached);

    mutex_release(&line_mutex);

    *action = attached;

    return 0;
}

int interrupt_attach_message(const interrupt_message_ops_t* restrict ops, void* restrict source, irq_handler_t handler,
    void* restrict ctxdata, interrupt_action_t** restrict action)
{
    interrupt_action_t* attached = kmalloc(sizeof(interrupt_action_t));

    if(attached == nullptr) {
        return -ENOMEM;
    }

    attached->handler = handler;
    attached->ctxdata = ctxdata;

    mutex_acquire(&line_mutex);

    interrupt_line_t* line = alloc_line();

    if(line == nullptr) {
        mutex_release(&line_mutex);
        kfree(attached);
        return -ENOMEM;
    }

    line->message_ops = ops;
    line->source = source;

    ops->mask(source);
    program_line(line);

    pr_verbose("intctl: message on hart %hu, vector 0x%02X", line->hart->index, line->irq_number + ARCH_IRQ_BASE);
    add_action(line, attached);

    mutex_release(&line_mutex);

    *action = attached;

    return 0;
}

void interrupt_detach(interrupt_action_t* restrict action)
{
    interrupt_line_t* line = action->line;

    mutex_acquire(&line_mutex);

    bool intr_state = spinlock_acquire_intr(&line->lock);

    interrupt_action_t** prev = &line->actions;

    while(*prev != action) {
        prev = &(*prev)->next;
    }

    *prev = action->next;

    spinlock_release_intr(&line->lock, intr_state);

    if(line->actions == nullptr) {
        destroy_line(line);
    }

    mutex_release(&line_mutex);

    kfree(action);
}
