#include <vex/assert.h>
#include <vex/cmdline.h>
#include <vex/crc32_iso3309.h>
#include <vex/ctype.h>
#include <vex/dev.h>
#include <vex/dev_name.h>
#include <vex/device.h>
#include <vex/errno.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/panic.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/sprintf.h>
#include <vex/stdlib.h>
#include <vex/string.h>

static_assert(sizeof(dev_unit_t) <= SLAB_MAX_SIZE);

static spinlock_t dev_units_lock = SPINLOCK_INIT;

dev_prefix_t dev_prefix_anon = {
    .string = "anon",
};

dev_prefix_t dev_prefix_ata = {
    .string = "ada",
};

dev_prefix_t dev_prefix_atapi = {
    .string = "acd",
};

dev_prefix_t dev_prefix_nvme = {
    .string = "nvd",
};

dev_prefix_t dev_prefix_virtio = {
    .string = "vbd",
};

dev_prefix_t dev_prefix_tty = {
    .string = "tty",
};

dev_prefix_t dev_prefix_pty = {
    .string = "pty",
};

dev_prefix_t dev_prefix_serial = {
    .string = "serial",
};

dev_prefix_t dev_prefix_serialtest = {
    .string = "serialtest",
};

dev_prefix_t dev_prefix_fbd = {
    .string = "fbd",
};

dev_prefix_t dev_prefix_kplog = {
    .string = "kplog",
};

dev_prefix_t dev_prefix_hwtree = {
    .string = "hwtree",
};

static dev_prefix_t* const dev_prefixes[] = {
    &dev_prefix_anon,
    &dev_prefix_ata,
    &dev_prefix_atapi,
    &dev_prefix_nvme,
    &dev_prefix_virtio,
    &dev_prefix_tty,
    &dev_prefix_pty,
    &dev_prefix_serial,
    &dev_prefix_serialtest,
    &dev_prefix_fbd,
    &dev_prefix_kplog,
    &dev_prefix_hwtree,
};

static void link_unit_locked(dev_prefix_t* restrict prefix, dev_unit_t* restrict unit, dev_unit_t* restrict prev, unsigned number)
{
    unit->prefix = prefix;
    unit->number = number;
    unit->prev = prev;
    unit->next = prev ? prev->next : prefix->units;

    if(unit->next) {
        unit->next->prev = unit;
    }

    if(prev) {
        prev->next = unit;
    }
    else {
        prefix->units = unit;
    }
}

// unit=ada0,acpi:PCI0/pci:0000:00:1f.2 pins ada0 to the node with that path, as /dev/hwtree shows it
static bool parse_pinned(const char* restrict value, size_t* restrict prefix_length, unsigned* restrict number, const char** restrict path)
{
    size_t letters = 0;

    while(isalpha(value[letters])) {
        letters += 1;
    }

    const char* end;
    unsigned long long parsed = strtoull(&value[letters], &end, 10);

    bool malformed = letters == 0 || end == &value[letters];

    if(malformed || *end != ',' || parsed >= DEV_NO_UNIT) {
        return false;
    }

    *prefix_length = letters;
    *number = parsed;
    *path = end + 1;

    return true;
}

static bool pinned_prefix_is(const dev_prefix_t* restrict prefix, const char* restrict value, size_t prefix_length)
{
    bool result = true;
    result = result && prefix_length && strlen(prefix->string);
    result = result && 0 == strncmp(value, prefix->string, prefix_length);
    return result;
}

// A null path asks whether any node keeps the number
static bool is_pinned(const dev_prefix_t* restrict prefix, unsigned number, const char* restrict path)
{
    const char* value = cmdline_next_argument(&sys_cmdline, "unit", nullptr);

    while(value) {
        size_t prefix_length;
        unsigned pinned;
        const char* pinned_path;

        bool matches = parse_pinned(value, &prefix_length, &pinned, &pinned_path);
        matches = matches && pinned_prefix_is(prefix, value, prefix_length);
        matches = matches && pinned == number;
        matches = matches && (path == nullptr || 0 == strcmp(pinned_path, path));

        if(matches) {
            return true;
        }

        value = cmdline_next_argument(&sys_cmdline, "unit", value);
    }

    return false;
}

static unsigned pinned_number(const dev_prefix_t* restrict prefix, const char* restrict path)
{
    const char* value = cmdline_next_argument(&sys_cmdline, "unit", nullptr);

    while(value) {
        size_t prefix_length;
        unsigned pinned;
        const char* pinned_path;

        bool matches = parse_pinned(value, &prefix_length, &pinned, &pinned_path);
        matches = matches && pinned_prefix_is(prefix, value, prefix_length);
        matches = matches && 0 == strcmp(pinned_path, path);

        if(matches) {
            return pinned;
        }

        value = cmdline_next_argument(&sys_cmdline, "unit", value);
    }

    return DEV_NAME_NONE;
}

// Sets prev to the last unit below the number, where a new unit with the number links in
static bool unit_taken_locked(const dev_prefix_t* restrict prefix, unsigned number, dev_unit_t** restrict prev)
{
    *prev = nullptr;

    for(dev_unit_t* it = prefix->units; it && it->number <= number; it = it->next) {
        if(it->number == number) {
            return true;
        }

        *prev = it;
    }

    return false;
}

int dev_prefix_claim(dev_prefix_t* restrict prefix)
{
    int retval = 0;

    spinlock_acquire(&dev_units_lock);

    if(prefix->claimed || prefix->units) {
        retval = -EEXIST;
    }
    else {
        prefix->claimed = true;
    }

    spinlock_release(&dev_units_lock);

    return retval;
}

void dev_prefix_release(dev_prefix_t* restrict prefix)
{
    spinlock_acquire(&dev_units_lock);

    kassert(prefix->claimed);
    prefix->claimed = false;

    spinlock_release(&dev_units_lock);
}

int dev_unit_alloc(dev_prefix_t* restrict prefix, const device_t* restrict device, dev_unit_t** restrict unit)
{
    char path[DEVICE_PATH_MAX];
    unsigned pinned = DEV_NAME_NONE;

    if(device) {
        device_get_path(device, path, sizeof(path));
        pinned = pinned_number(prefix, path);
    }

    dev_unit_t* created = slab_alloc(sizeof(dev_unit_t));

    if(created == nullptr) {
        return -ENOMEM;
    }

    spinlock_acquire(&dev_units_lock);

    if(prefix->claimed) {
        spinlock_release(&dev_units_lock);
        slab_free(created);
        return -EEXIST;
    }

    dev_unit_t* prev;
    unsigned number = pinned;
    bool pinned_taken = pinned != DEV_NAME_NONE && unit_taken_locked(prefix, pinned, &prev);

    if(pinned == DEV_NAME_NONE || pinned_taken) {
        number = 0;

        while(number < DEV_NO_UNIT && (unit_taken_locked(prefix, number, &prev) || is_pinned(prefix, number, nullptr))) {
            number += 1;
        }
    }

    if(number >= DEV_NO_UNIT) {
        spinlock_release(&dev_units_lock);
        slab_free(created);
        return -ENOSPC;
    }

    link_unit_locked(prefix, created, prev, number);

    spinlock_release(&dev_units_lock);

    if(pinned_taken) {
        pr_warning("dev_name: %s%u is taken, %s gets %s%u", prefix->string, pinned, path, prefix->string, number);
    }

    *unit = created;

    return 0;
}

int dev_unit_claim(dev_prefix_t* restrict prefix, unsigned number, dev_unit_t** restrict unit)
{
    if(number >= DEV_NO_UNIT) {
        return -ERANGE;
    }

    dev_unit_t* created = slab_alloc(sizeof(dev_unit_t));

    if(created == nullptr) {
        return -ENOMEM;
    }

    spinlock_acquire(&dev_units_lock);

    dev_unit_t* prev;

    if(prefix->claimed || unit_taken_locked(prefix, number, &prev)) {
        spinlock_release(&dev_units_lock);
        slab_free(created);
        return -EEXIST;
    }

    link_unit_locked(prefix, created, prev, number);

    spinlock_release(&dev_units_lock);

    *unit = created;

    return 0;
}

void dev_unit_free(dev_unit_t* restrict unit)
{
    spinlock_acquire(&dev_units_lock);

    if(unit->next) {
        unit->next->prev = unit->prev;
    }

    if(unit->prev) {
        unit->prev->next = unit->next;
    }
    else {
        unit->prefix->units = unit->next;
    }

    spinlock_release(&dev_units_lock);

    slab_free(unit);
}

int dev_name_format(const dev_name_t* restrict name, char* restrict buffer, size_t size)
{
    kassert(name->unit != DEV_NAME_NONE || name->index == DEV_NAME_NONE);
    kassert(name->index != DEV_NAME_NONE || name->partition == DEV_NAME_NONE);

    if(name->unit != DEV_NAME_NONE && name->unit >= DEV_NO_UNIT) {
        return -ERANGE;
    }

    if(name->index != DEV_NAME_NONE && name->index >= DEV_NO_INDEX) {
        return -ERANGE;
    }

    if(name->partition != DEV_NAME_NONE && name->partition >= DEV_NO_PARTITION) {
        return -ERANGE;
    }

    const char* prefix = name->prefix->string;
    int length;

    if(name->unit == DEV_NAME_NONE) {
        length = snprintf(buffer, size, "%s", prefix);
    }
    else if(name->index == DEV_NAME_NONE) {
        length = snprintf(buffer, size, "%s%u", prefix, name->unit);
    }
    else if(name->partition == DEV_NAME_NONE) {
        length = snprintf(buffer, size, "%s%un%u", prefix, name->unit, name->index);
    }
    else {
        length = snprintf(buffer, size, "%s%un%up%u", prefix, name->unit, name->index, name->partition);
    }

    if(length < 0 || (size_t)(length) >= size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

dev_t dev_name_to_dev(const dev_name_t* restrict name)
{
    unsigned unit = name->unit == DEV_NAME_NONE ? DEV_NO_UNIT : name->unit;
    unsigned index = name->index == DEV_NAME_NONE ? DEV_NO_INDEX : name->index;
    unsigned partition = name->partition == DEV_NAME_NONE ? DEV_NO_PARTITION : name->partition;
    return DEV_SET(name->prefix->hash, unit, index, partition);
}

static void init_dev_prefixes(void)
{
    for(size_t i = 0; i < sizeof(dev_prefixes) / sizeof(dev_prefixes[0]); ++i) {
        dev_prefix_t* prefix = dev_prefixes[i];
        prefix->hash = crc32_iso3309_compute(prefix->string, strlen(prefix->string));

        for(size_t j = 0; j < i; ++j) {
            if(dev_prefixes[j]->hash == prefix->hash) {
                panic("dev_name: %s and %s have the same hash", dev_prefixes[j]->string, prefix->string);
            }
        }
    }
}

initcall_bsp_early(init_dev_prefixes, INIT_LATE);

static void init_pinned_units(void)
{
    const char* value = cmdline_next_argument(&sys_cmdline, "unit", nullptr);

    while(value) {
        size_t prefix_length;
        unsigned number;
        const char* path;
        bool known = false;

        if(parse_pinned(value, &prefix_length, &number, &path)) {
            for(size_t i = 0; i < sizeof(dev_prefixes) / sizeof(dev_prefixes[0]) && !known; ++i) {
                known = pinned_prefix_is(dev_prefixes[i], value, prefix_length);
            }
        }

        if(!known) {
            pr_warning("dev_name: bad unit=%s", value);
        }

        value = cmdline_next_argument(&sys_cmdline, "unit", value);
    }
}

initcall_bsp_bus(init_pinned_units, INIT_FIRST);
