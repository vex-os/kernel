#pragma once

#include <vex/compiler.h>
#include <vex/limits.h>
#include <vex/types.h>

typedef struct device_s device_t;

#define DEV_NAME_NONE UINT_MAX // A unit, index or partition the name doesn't have

typedef struct dev_prefix_s dev_prefix_t;
typedef struct dev_unit_s dev_unit_t;
typedef struct dev_name_s dev_name_t;

struct dev_prefix_s {
    dev_unit_t* units; // Sorted by number
    const char* string;
    uint32_t hash; // The prefix field of a dev_t
    bool claimed;  // One device has the prefix alone as its name, so no unit can be taken
};

struct dev_unit_s {
    dev_unit_t* prev;
    dev_unit_t* next;
    dev_prefix_t* prefix;
    unsigned number;
};

struct dev_name_s {
    dev_prefix_t* prefix;
    unsigned unit;
    unsigned index;
    unsigned partition;
};

extern dev_prefix_t dev_prefix_anon;
extern dev_prefix_t dev_prefix_ata;
extern dev_prefix_t dev_prefix_atapi;
extern dev_prefix_t dev_prefix_nvme;
extern dev_prefix_t dev_prefix_virtio;
extern dev_prefix_t dev_prefix_tty;
extern dev_prefix_t dev_prefix_pty;
extern dev_prefix_t dev_prefix_serial;
extern dev_prefix_t dev_prefix_serialtest;
extern dev_prefix_t dev_prefix_fbd;
extern dev_prefix_t dev_prefix_kplog;
extern dev_prefix_t dev_prefix_hwtree;
extern dev_prefix_t dev_prefix_random;
extern dev_prefix_t dev_prefix_urandom;

/// Takes the prefix alone as the name of one device
/// @retval -EEXIST Another device has the prefix, or a unit under it was taken
int dev_prefix_claim(dev_prefix_t* restrict prefix);

/// @pre The device named with the prefix is removed
void dev_prefix_release(dev_prefix_t* restrict prefix);

/// Takes the number that a unit= option gives the node, else the lowest number that no unit has and no unit= option keeps
/// @param device The node of the hardware, or nullptr for a unit without one
/// @retval -ENOSPC Every number a dev_t can hold is taken
/// @retval -EEXIST A device has the prefix alone as its name
int dev_unit_alloc(dev_prefix_t* restrict prefix, const device_t* restrict device, dev_unit_t** restrict unit);

/// Takes the given number, such as one the hardware assigns
/// @retval -EEXIST Another unit has the number, or a device has the prefix alone as its name
/// @retval -ERANGE A dev_t can't hold the number
int dev_unit_claim(dev_prefix_t* restrict prefix, unsigned number, dev_unit_t** restrict unit);

/// @pre The devices named with the unit are removed
void dev_unit_free(dev_unit_t* restrict unit);

/// @retval -ERANGE A dev_t can't hold the unit, index or partition
/// @retval -ENAMETOOLONG The buffer is too small
int dev_name_format(const dev_name_t* restrict name, char* restrict buffer, size_t size);

dev_t dev_name_to_dev(const dev_name_t* restrict name) __nodiscard;
