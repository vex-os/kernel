#pragma once

#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/dev_name.h>
#include <vex/errno.h>
#include <vex/types.h>

enum {
    BLOCK_REQ_READ,
    BLOCK_REQ_WRITE,
    BLOCK_REQ_FLUSH,
};

typedef struct device_s device_t;

#define BLOCK_FLAG_PRESENT   0x0001 // Media is present and ready
#define BLOCK_FLAG_PARTITION 0x0002
#define BLOCK_FLAG_HIDDEN    0x0004 // Device is not added to devfs

typedef struct block_segment_s block_segment_t;
typedef struct block_request_s block_request_t;
typedef struct block_interface_s block_interface_t;
typedef struct block_interface_ops_s block_interface_ops_t;
typedef struct block_interface_private_s block_interface_private_t;

struct block_segment_s {
    phys_addr_t phys;
    size_t length;
};

/// Called once per request with status set, possibly before block_submit returns
typedef void (*block_request_done_t)(block_request_t* restrict req);

struct block_request_s {
    unsigned op;
    uintptr_t lba;
    size_t length;
    const block_segment_t* segments;
    size_t num_segments;
    int status;
    block_request_done_t done;
    void* ctxdata;
};

struct block_interface_ops_s {
    /// @pre block_submit has checked the request against the device's limits and found the device present
    /// @note Finish the request by setting status and calling done exactly once, failure included
    void (*submit)(const block_interface_t* restrict interface, block_request_t* restrict req);
    void (*release)(const block_interface_t* restrict interface);
};

struct block_interface_private_s {
    block_interface_t* next;
    dev_name_t name;
    dev_t dev;
    unsigned refcount;
};

struct block_interface_s {
    block_interface_private_t priv;
    const block_interface_ops_t* ops;
    char name[64];
    size_t sector_count;
    size_t sector_size;
    size_t max_transfer;
    size_t max_segments;
    phys_addr_t dma_limit; // Highest physical address a segment may reach
    uintptr_t dma_align;   // A mask: segment addresses and lengths have these bits clear
    unsigned flags;
    device_t* device; // The node of the hardware, set before the add; a partition takes the one of its disk
    void* ctxdata;
};

/// @param index The unit number in the name, such as the port of the controller
int block_interface_add_root(block_interface_t* restrict interface, dev_unit_t* restrict unit, unsigned index);

/// @param partition The partition number in the name, counted from one
int block_interface_add_part(block_interface_t* restrict interface, const block_interface_t* restrict root, unsigned partition);

/// Takes the device out of the registry and drops the reference it held there; holders of
/// other references keep the structure, but their requests fail with -ENODEV from now on
int block_interface_remove(block_interface_t* restrict interface);

void block_interface_incref(const block_interface_t* restrict interface);
void block_interface_decref(const block_interface_t* restrict interface);

const block_interface_t* block_interface_find(const char* restrict name) __nodiscard;

/// @pre The caller holds block_event_lock, so the device list does not change
void block_interface_for_each(void (*callback)(const block_interface_t* restrict interface, void* restrict arg), void* restrict arg);

/// Starts the request and returns; done reports the outcome, even for a request
/// refused right away as -ENODEV or -EINVAL. Segments are physical addresses
/// within dma_limit and dma_align, the length a multiple of the sector size
void block_submit(const block_interface_t* restrict interface, block_request_t* restrict req);

/// block_submit, then waits for done; replaces whatever done and ctxdata the request had
/// @pre Interrupts are enabled, unless the driver finishes the request before block_submit returns
/// @return The request's status
int block_submit_wait(const block_interface_t* restrict interface, block_request_t* restrict req);

/// Reads synchronously, in as many requests within the device's limits as the transfer needs
/// @pre Interrupts are enabled, unless the driver finishes requests before block_submit returns
/// @param count Sectors to transfer
/// @param buffer Any kernel memory; pages the device can't reach by DMA go through a bounce buffer
/// @retval -ENOMEM No memory for the bounce buffer
int block_read(const block_interface_t* restrict interface, uintptr_t lba, size_t count, void* restrict buffer);

/// Writes synchronously, in as many requests within the device's limits as the transfer needs
/// @pre Interrupts are enabled, unless the driver finishes requests before block_submit returns
/// @param count Sectors to transfer
/// @param buffer Any kernel memory; pages the device can't reach by DMA go through a bounce buffer
/// @retval -ENOMEM No memory for the bounce buffer
int block_write(const block_interface_t* restrict interface, uintptr_t lba, size_t count, const void* restrict buffer);

int block_flush(const block_interface_t* restrict interface);
int block_read_sector_prefix(const block_interface_t* restrict interface, uintptr_t lba, void* restrict buffer, size_t size);
