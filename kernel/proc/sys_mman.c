#include <vex/capability.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/file.h>
#include <vex/mman.h>
#include <vex/mount.h>
#include <vex/page.h>
#include <vex/pmap.h>
#include <vex/process.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/vms.h>

#include "syscall_internal.h"

#define KNOWN_PROT    (PROT_READ | PROT_WRITE | PROT_EXEC)
#define KNOWN_FLAGS   (MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE | MAP_POPULATE | MAP_STACK | MAP_FIXED_NOREPLACE)
#define KNOWN_SYNC    (MS_ASYNC | MS_INVALIDATE | MS_SYNC)
#define KNOWN_LOCKALL (MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)

static unsigned prot_to_vprot(uintptr_t prot)
{
    unsigned vprot = VPROT_NONE;

    if(prot & PROT_READ) {
        vprot |= VPROT_READ;
    }

    if(prot & PROT_WRITE) {
        vprot |= VPROT_WRITE;
    }

    if(prot & PROT_EXEC) {
        vprot |= VPROT_EXEC;
    }

    return vprot;
}

static bool user_range_end(vmspace_t* restrict vmspace, uintptr_t start, uintptr_t length, uintptr_t* restrict end)
{
    uintptr_t user_end = pmap_user_end(vmspace->pmap);

    if(length == 0 || page_offset(start) || start >= user_end || length > user_end - start) {
        return false;
    }

    *end = start + page_align_up(length);

    return *end <= user_end;
}

static intptr_t map_range(vmspace_t* restrict vmspace, uintptr_t addr, uintptr_t length, uintptr_t flags, unsigned vprot,
    unsigned vms_flags, const vm_backing_t* restrict backing)
{
    uintptr_t end;

    if(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if(!user_range_end(vmspace, addr, length, &end)) {
            return -EINVAL;
        }

        int retval;

        if(flags & MAP_FIXED_NOREPLACE) {
            retval = vms_map(vmspace, addr, end, vprot, vms_flags, backing);
        }
        else {
            retval = vms_map_replace(vmspace, addr, end, vprot, vms_flags, backing);
        }

        if(retval < 0) {
            return retval;
        }

        return addr;
    }

    // The address is a hint taken when it is free
    if(addr && user_range_end(vmspace, addr, length, &end) && 0 == vms_map(vmspace, addr, end, vprot, vms_flags, backing)) {
        return addr;
    }

    uintptr_t placed;
    int retval = vms_map_any(vmspace, page_align_up(length), vprot, vms_flags, backing, &placed);

    if(retval < 0) {
        return retval;
    }

    return placed;
}

static intptr_t place_mapping(vmspace_t* restrict vmspace, uintptr_t addr, uintptr_t length, uintptr_t flags, unsigned vprot,
    unsigned vms_flags, const vm_backing_t* restrict backing)
{
    intptr_t placed = map_range(vmspace, addr, length, flags, vprot, vms_flags, backing);

    if(placed >= 0 && (flags & MAP_POPULATE)) {
        vms_populate(vmspace, placed, placed + page_align_up(length));
    }

    return placed;
}

static intptr_t sys_mmap(uintptr_t addr, uintptr_t length, uintptr_t prot, uintptr_t flags, int fd, uintptr_t offset)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(length == 0 || length > pmap_user_end(process->vmspace->pmap) || (prot & ~KNOWN_PROT) || (flags & ~KNOWN_FLAGS)) {
        return -EINVAL;
    }

    uintptr_t sharing = flags & (MAP_SHARED | MAP_PRIVATE);

    if(sharing != MAP_SHARED && sharing != MAP_PRIVATE) {
        return -EINVAL;
    }

    unsigned vprot = prot_to_vprot(prot);
    unsigned vms_flags = 0;

    // Without the capability the flag does nothing, as programs pass it out of habit
    if((flags & MAP_NORESERVE) && cred_capable(thread->credentials, CAP_VM_NORESERVE)) {
        vms_flags |= VMS_NORESERVE;
    }

    if(flags & MAP_ANONYMOUS) {
        if(sharing == MAP_SHARED) {
            return -ENOSYS; // Shared anonymous memory only differs from private across fork
        }

        return place_mapping(process->vmspace, addr, length, flags, vprot, vms_flags, nullptr);
    }

    if(page_offset(offset) || (off_t)(offset) < 0) {
        return -EINVAL;
    }

    file_t* file = process_fd_get(process, fd);

    if(file == nullptr) {
        return -EBADF;
    }

    bool device = file->node->ops->resolve_page;
    bool shared = sharing == MAP_SHARED;
    bool readonly = !device && file->node->fs->readonly; // A device node stays writable on a read-only filesystem
    bool may_write = !shared || (file_flags_allow_write(file->flags) && !readonly);
    intptr_t retval;

    if(file->node->type != VNODE_FILE && !device) {
        retval = -ENODEV;
    }
    else if(device && !shared) {
        retval = -EINVAL; // Device memory has no pages of its own to copy on write
    }
    else if(!file_flags_allow_read(file->flags)) {
        retval = -EACCES;
    }
    else if((prot & PROT_WRITE) && !may_write) {
        retval = readonly ? -EROFS : -EACCES;
    }
    else {
        retval = vnode_mmap(file->node, offset, page_align_up(length));

        if(retval == 0) {
            vm_backing_t backing;
            backing.file = file;
            backing.file_offset = offset;
            backing.shared = shared;
            backing.may_write = may_write;
            backing.device = device;

            retval = place_mapping(process->vmspace, addr, length, flags, vprot, vms_flags, &backing);
        }
    }

    file_close(file);

    return retval;
}

define_syscall(sys_mmap, SYS_MMAP, 6);

static intptr_t sys_munmap(uintptr_t addr, uintptr_t length)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    uintptr_t end;

    if(!user_range_end(process->vmspace, addr, length, &end)) {
        return -EINVAL;
    }

    return vms_unmap(process->vmspace, addr, end);
}

define_syscall(sys_munmap, SYS_MUNMAP, 2);

static intptr_t sys_mprotect(uintptr_t addr, uintptr_t length, uintptr_t prot)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    uintptr_t end;

    if(!user_range_end(process->vmspace, addr, length, &end) || (prot & ~KNOWN_PROT)) {
        return -EINVAL;
    }

    return vms_protect(process->vmspace, addr, end, prot_to_vprot(prot));
}

define_syscall(sys_mprotect, SYS_MPROTECT, 3);

static intptr_t sys_msync(uintptr_t addr, uintptr_t length, uintptr_t flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    uintptr_t end;

    if(!user_range_end(process->vmspace, addr, length, &end) || (flags & ~KNOWN_SYNC) || ((flags & MS_ASYNC) && (flags & MS_SYNC))) {
        return -EINVAL;
    }

    return vms_sync(process->vmspace, addr, end, (flags & MS_SYNC));
}

define_syscall(sys_msync, SYS_MSYNC, 3);

// The range is rounded out to whole pages, as Linux does, and has to be mapped as a whole
static intptr_t check_range(vmspace_t* restrict vmspace, uintptr_t addr, uintptr_t length)
{
    uintptr_t user_end = pmap_user_end(vmspace->pmap);

    if(addr >= user_end || length > user_end - addr) {
        return -ENOMEM;
    }

    return vms_check_mapped(vmspace, page_align(addr), page_align_up(addr + length));
}

// UNDONE: MADV_WILLNEED reads ahead
static intptr_t sys_madvise(uintptr_t addr, uintptr_t length, uintptr_t advice)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    switch(advice) {
        case MADV_NORMAL:
        case MADV_RANDOM:
        case MADV_SEQUENTIAL:
        case MADV_WILLNEED:
        case MADV_DONTNEED:
            break;

        default:
            return -EINVAL;
    }

    if(page_offset(addr)) {
        return -EINVAL;
    }

    if(length == 0) {
        return 0;
    }

    intptr_t retval = check_range(process->vmspace, addr, length);

    if(retval == 0 && advice == MADV_DONTNEED) {
        return vms_discard(process->vmspace, addr, page_align_up(addr + length));
    }

    return retval;
}

define_syscall(sys_madvise, SYS_MADVISE, 3);

// UNDONE: Locked pages matter once there is swap. Then
// this needs a capability and a limit, with RLIMIT_MEMLOCK
static intptr_t sys_mlock(uintptr_t addr, uintptr_t length)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(length == 0) {
        return 0;
    }

    return check_range(process->vmspace, addr, length);
}

define_syscall(sys_mlock, SYS_MLOCK, 2);

static intptr_t sys_munlock(uintptr_t addr, uintptr_t length)
{
    return sys_mlock(addr, length); // Nothing is locked, so unlocking only checks the range
}

define_syscall(sys_munlock, SYS_MUNLOCK, 2);

// UNDONE: As for mlock, and MCL_FUTURE has to reach the mappings made later
static intptr_t sys_mlockall(uintptr_t flags)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    // MCL_ONFAULT only says how to lock, so it needs one of the others
    if((flags & ~(uintptr_t)(KNOWN_LOCKALL)) || 0 == (flags & (MCL_CURRENT | MCL_FUTURE))) {
        return -EINVAL;
    }

    return 0;
}

define_syscall(sys_mlockall, SYS_MLOCKALL, 1);

static intptr_t sys_munlockall(void)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    return 0;
}

define_syscall(sys_munlockall, SYS_MUNLOCKALL, 0);
