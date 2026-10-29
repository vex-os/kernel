#include <vex/cmdline.h>
#include <vex/device.h>
#include <vex/fwnode.h>
#include <vex/initcall.h>
#include <vex/intctl.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/stdlib.h>
#include <vex/string.h>

#define FIXED_OPTION_ID_MAX        16
#define FIXED_OPTION_RESOURCES_MAX 8

typedef struct fixed_option_s fixed_option_t;

struct fixed_option_s {
    fixed_option_t* next;
    fwnode_fixed_t fixed;
    bool found; // A node with the parent's path synced its children, so the option isn't lost
    char parent_path[DEVICE_PATH_MAX];
    char id[FIXED_OPTION_ID_MAX];
    fwnode_resource_t resources[FIXED_OPTION_RESOURCES_MAX];
};

// Written once at boot and never changed, so the walks take no lock
static fixed_option_t* fixed_options = nullptr;

static bool parse_number(const char* restrict text, const char** restrict end, uint64_t* restrict number)
{
    *number = strtoull(text, end, 0);

    return *end != text;
}

// io:0x2e8+8 and mem:0xd0000+0x1000 give a start and a length
static bool parse_range(const char* restrict text, unsigned type, fwnode_resource_t* restrict resource)
{
    const char* end;
    uint64_t start;
    uint64_t length;

    bool parsed = parse_number(text, &end, &start);
    parsed = parsed && *end == '+';
    parsed = parsed && parse_number(end + 1, &end, &length);
    parsed = parsed && (*end == ',' || *end == 0);
    parsed = parsed && length;

    if(!parsed) {
        return false;
    }

    resource->type = type;
    resource->range.start = start;
    resource->range.length = length;
    resource->range.translation = 0;
    resource->range.flags = 0;

    return true;
}

static bool parse_irq(const char* restrict text, unsigned flags, fwnode_resource_t* restrict resource)
{
    const char* end;
    uint64_t number;

    if(!parse_number(text, &end, &number)) {
        return false;
    }

    bool trailing = *end != ',' && *end;

    if(trailing || number > UINT32_MAX) {
        return false;
    }

    resource->type = FWNODE_RESOURCE_IRQ;
    resource->irq.number = number;
    resource->irq.trigger = TRIGGER_EDGE;
    resource->irq.polarity = ACTIVE_HIGH;
    resource->irq.flags = flags;

    return true;
}

static bool parse_dma(const char* restrict text, fwnode_resource_t* restrict resource)
{
    const char* end;
    uint64_t channel;

    if(!parse_number(text, &end, &channel)) {
        return false;
    }

    bool trailing = *end != ',' && *end;

    if(trailing || channel > UINT8_MAX) {
        return false;
    }

    resource->type = FWNODE_RESOURCE_DMA;
    resource->dma_channel = channel;

    return true;
}

static bool parse_resource(const char* restrict field, fwnode_resource_t* restrict resource)
{
    if(0 == strncmp(field, "io:", 3)) {
        return parse_range(&field[3], FWNODE_RESOURCE_PORT, resource);
    }

    if(0 == strncmp(field, "mem:", 4)) {
        return parse_range(&field[4], FWNODE_RESOURCE_MEMORY, resource);
    }

    if(0 == strncmp(field, "irq:", 4)) {
        return parse_irq(&field[4], FWNODE_IRQ_ISA, resource);
    }

    if(0 == strncmp(field, "gsi:", 4)) {
        return parse_irq(&field[4], 0, resource);
    }

    if(0 == strncmp(field, "dma:", 4)) {
        return parse_dma(&field[4], resource);
    }

    return false;
}

// fixed=acpi:PCI0/pci:0000:00:1f.0/PNP0501,io:0x2e8+8,irq:3 puts a PNP0501 under that node;
// without a slash before the first comma, the node goes under the root
static bool parse_option(const char* restrict value, fixed_option_t* restrict option)
{
    size_t head_length = strcspn(value, ",");
    size_t slash = head_length;

    while(slash && value[slash - 1] != '/') {
        slash -= 1;
    }

    size_t parent_length = slash ? slash - 1 : 0;
    size_t id_length = head_length - slash;

    if(id_length == 0 || id_length >= sizeof(option->id) || parent_length >= sizeof(option->parent_path)) {
        return false;
    }

    memcpy(option->parent_path, value, parent_length);
    option->parent_path[parent_length] = 0;

    memcpy(option->id, &value[slash], id_length);
    option->id[id_length] = 0;

    size_t count = 0;
    const char* field = &value[head_length];

    while(*field) {
        field += 1;

        if(count == FIXED_OPTION_RESOURCES_MAX || !parse_resource(field, &option->resources[count])) {
            return false;
        }

        count += 1;
        field += strcspn(field, ",");
    }

    option->next = nullptr;
    option->found = false;
    option->fixed.name = option->id;
    option->fixed.id = option->id;
    option->fixed.resources = option->resources;
    option->fixed.num_resources = count;

    return true;
}

void fwnode_for_each_fixed_option(const char* restrict parent_path, void (*callback)(const fwnode_t* child, void* arg), void* arg)
{
    fixed_option_t* option = fixed_options;

    while(option) {
        if(0 == strcmp(option->parent_path, parent_path)) {
            fwnode_t node;
            fwnode_init_fixed(&node, &option->fixed);

            option->found = true;
            callback(&node, arg);
        }

        option = option->next;
    }
}

static void init_fixed_options(void)
{
    fixed_option_t** tail = &fixed_options;
    const char* value = cmdline_next_argument(&sys_cmdline, "fixed", nullptr);

    while(value) {
        fixed_option_t* option = kmalloc(sizeof(fixed_option_t));

        if(option == nullptr) {
            pr_warning("fixed: out of memory");
            return;
        }

        if(parse_option(value, option)) {
            *tail = option;
            tail = &option->next;
        }
        else {
            pr_warning("fixed: bad fixed=%s", value);
            kfree(option);
        }

        value = cmdline_next_argument(&sys_cmdline, "fixed", value);
    }
}

initcall_bsp_bus(init_fixed_options, INIT_FIRST);

static void report_lost_options(void)
{
    fixed_option_t* option = fixed_options;

    while(option) {
        if(!option->found) {
            pr_warning("fixed: %s/%s: no node has the path", option->parent_path, option->id);
        }

        option = option->next;
    }
}

initcall_bsp_late(report_lost_options, INIT_LAST);
