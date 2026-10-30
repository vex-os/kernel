#pragma once

#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/mutex.h>
#include <vex/types.h>

typedef struct file_s file_t;
typedef struct pmap_s pmap_t;
typedef struct vnode_s vnode_t;

#define VMS_MMAP_TOP UINT64_C(0x0000600000000000) // Placement without a fixed address searches down from here
#define VMS_MMAP_MIN UINT64_C(0x0000000000010000) // Keeps null pointer dereferences faulting

#define VMS_NORESERVE 0x1 // The region takes nothing from the commit limit

#define VMS_FAULT_MINOR 0 // The page was in memory, or a fresh one did
#define VMS_FAULT_MAJOR 1 // The page had to come from the backing store of a file

typedef struct vm_backing_s vm_backing_t;
typedef struct vm_region_s vm_region_t;
typedef struct vmspace_s vmspace_t;

struct vm_backing_s {
    file_t* file;
    off_t file_offset; // Of the start of the range, page aligned
    bool shared;
    bool may_write; // Whether a shared mapping can ever be made writable
    bool device;    // Device memory: shared, mapped as the driver says, without pframes
};

struct vm_region_s {
    vm_region_t* next; // Sorted by start
    virt_addr_t start; // Page aligned, inclusive
    virt_addr_t end;   // Page aligned, exclusive
    unsigned vprot;    // Without VPROT_USER, which every user page has
    file_t* file;      // Holds a reference, so the mapping keeps the file open; nullptr for anonymous memory
    off_t file_offset;
    bool shared;
    bool may_write;
    bool device;
    bool noreserve;
    bool grows_down;          // A stack: a fault under it moves start down, as far as grow_floor
    virt_addr_t grow_floor;   // The lowest start of a stack
    vm_region_t* device_next; // In the list of device regions vms_revoke walks
    pmap_t* device_pmap;      // The page table of a device region, for vms_revoke
};

struct vmspace_s {
    vm_region_t* regions;
    pmap_t* pmap;
    mutex_t lock;
};

vmspace_t* vms_create(void) __nodiscard;

/// @pre No hart has the address space loaded
/// @note Nothing flushes the TLB entries that the address space left
void vms_release(vmspace_t* restrict vmspace);

/// Maps private anonymous memory that grows down on a fault under it, as far as floor
/// @note The growth takes its pages from the commit limit only when it happens, and fails with a fault
/// that finds no memory. A gap of free address space has to stay under the stack for it to grow
int vms_map_stack(vmspace_t* restrict vmspace, virt_addr_t floor, virt_addr_t start, virt_addr_t end, unsigned vprot);

/// @retval -EEXIST The range overlaps an existing mapping; vms_map_replace unmaps it instead
int vms_map(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, unsigned vprot, unsigned flags,
    const vm_backing_t* restrict backing);

/// Maps the range, unmapping whatever was mapped there before
int vms_map_replace(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, unsigned vprot, unsigned flags,
    const vm_backing_t* restrict backing);

int vms_map_any(vmspace_t* restrict vmspace, size_t size, unsigned vprot, unsigned flags, const vm_backing_t* restrict backing,
    virt_addr_t* restrict out);
int vms_unmap(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end);
/// Drops the pages of the range and keeps the regions, so the next access faults a fresh page in:
/// zeros for anonymous memory, the contents of the file for a file mapping
/// @retval -EINVAL The range is not page aligned, empty, or not in user space, or it holds device memory
/// @retval -ENOMEM Part of the range is not mapped
int vms_discard(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end);

/// Checks that every page of the range is mapped
/// @retval -EINVAL The range is not page aligned, empty, or not in user space
/// @retval -ENOMEM Part of the range is not mapped
int vms_check_mapped(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end);

/// Changes the size of the mapping that begins at start, as mremap does. Growing takes the space after it, or with may_move
/// a new place, to which the pages go with their contents and the mapping goes from the old one
/// @pre start, old_size and new_size are page aligned, and old_size and new_size are not zero
/// @param out Receives the address of the mapping afterwards
/// @retval -EFAULT The old range is not inside one mapping
/// @retval -EINVAL The mapping is device memory or a stack, or the old range is not in user space
/// @retval -ENOMEM There is no room to grow, or the commit limit is over
int vms_remap(vmspace_t* restrict vmspace, virt_addr_t start, size_t old_size, size_t new_size, bool may_move, virt_addr_t* restrict out);

int vms_sync(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, bool wait);
int vms_protect(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, unsigned vprot);
int vms_unmap_all(vmspace_t* restrict vmspace);
int vms_clone(vmspace_t* restrict vmspace, vmspace_t* restrict source);

/// @retval VMS_FAULT_MINOR The page is mapped, and nothing waited for a device
/// @retval VMS_FAULT_MAJOR The page is mapped, and the contents of a file came from its backing store
int vms_fault(vmspace_t* restrict vmspace, virt_addr_t virt, unsigned access);

/// Faults in every page of the range that is mapped, and stops at the first one that fails
/// @note Reports no error, as the pages fault in on use anyway
void vms_populate(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end);

/// Unmaps the device memory of node from every address space, so the next access faults on it again
/// @pre The node's resolve_page fails from now on, or that fault maps the page back
void vms_revoke(vnode_t* restrict node);

/// Faults the page in and makes a private one the address space's own, without pinning it
/// @pre Nothing runs in the address space, as in exec, or the frame may move right away
/// @return The physical address of virt, or zero when it can't be mapped
phys_addr_t vms_touch(vmspace_t* restrict vmspace, virt_addr_t virt) __nodiscard;

/// Keeps the page at virt in place until vms_unpin
/// @note Makes a private page the process's own first, so that a write cannot move the mapping to a copy
/// @return The physical address of virt, or zero when it isn't mapped readable
phys_addr_t vms_pin(vmspace_t* restrict vmspace, virt_addr_t virt) __nodiscard;

void vms_unpin(phys_addr_t phys);

ssize_t vms_copyin(void* restrict dst, vmspace_t* restrict vmspace, virt_addr_t src, size_t size);

/// Copies a NUL-terminated string of at most max_sz bytes, terminator included
/// @return The length of the string, without the terminator
/// @retval -ENAMETOOLONG No terminator within max_sz; dst holds a truncated, terminated copy
/// @retval -EFAULT Part of the string isn't mapped readable
/// @retval -EINVAL max_sz is zero
ssize_t vms_copyin_str(char* restrict dst, vmspace_t* restrict vmspace, virt_addr_t src, size_t max_sz);

ssize_t vms_copyout(vmspace_t* restrict vmspace, virt_addr_t dst, const void* restrict src, size_t size);
ssize_t vms_copyout_str(vmspace_t* restrict vmspace, virt_addr_t dst, const char* restrict src, size_t max_sz);
