#include <vex/binfmt.h>
#include <vex/cred.h>
#include <vex/elf.h>
#include <vex/errno.h>
#include <vex/file.h>
#include <vex/hhdm.h>
#include <vex/initcall.h>
#include <vex/page.h>
#include <vex/pmap.h>
#include <vex/string.h>
#include <vex/vms.h>
#include <vex/vprot.h>

#ifndef ELF64_ARCH
#define ELF64_ARCH EM_NONE
#endif

#define ELF64_STACK_TOP UINT64_C(0x0000700000000000)

static bool ehdr_valid(const elf64_ehdr_t* restrict ehdr)
{
    bool result = true;
    result = result && ehdr->e_ident[EI_MAG0] == ELFMAG0;
    result = result && ehdr->e_ident[EI_MAG1] == ELFMAG1;
    result = result && ehdr->e_ident[EI_MAG2] == ELFMAG2;
    result = result && ehdr->e_ident[EI_MAG3] == ELFMAG3;
    result = result && ehdr->e_ident[EI_CLASS] == ELFCLASS64;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    result = result && ehdr->e_ident[EI_DATA] == ELFDATA2LSB;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    result = result && ehdr->e_ident[EI_DATA] == ELFDATA2MSB;
#else
#error Unknown endianness
#endif

    result = result && ehdr->e_machine == ELF64_ARCH;
    result = result && ehdr->e_type == ET_EXEC;
    result = result && ehdr->e_phentsize == sizeof(elf64_phdr_t);
    result = result && ehdr->e_phnum > 0;

    return result;
}

static unsigned phdr_vprot(uint32_t p_flags)
{
    unsigned vprot = VPROT_USER;

    if(p_flags & PF_R)
        vprot |= VPROT_READ;
    if(p_flags & PF_W)
        vprot |= VPROT_WRITE;
    if(p_flags & PF_X)
        vprot |= VPROT_EXEC;

    return vprot;
}

static bool segment_loads_range(const elf64_phdr_t* restrict phdr, uint64_t offset, uint64_t size)
{
    if(offset < phdr->p_offset) {
        return false;
    }

    uint64_t skip = offset - phdr->p_offset;

    if(skip > phdr->p_filesz) {
        return false;
    }

    return size <= phdr->p_filesz - skip;
}

static int copy_page(vmspace_t* restrict vmspace, file_t* restrict file, const elf64_phdr_t* restrict phdr, uintptr_t page)
{
    phys_addr_t phys = vms_touch(vmspace, virt_addr(page));

    if(address_null(phys)) {
        return -ENOMEM;
    }

    uint8_t* kva = phys_to_virt(phys);

    uintptr_t page_end = page + PAGE_SIZE;
    uintptr_t file_end = phdr->p_vaddr + phdr->p_filesz;
    uintptr_t mem_end = phdr->p_vaddr + phdr->p_memsz;
    uintptr_t lo = page;

    if(lo < phdr->p_vaddr) {
        lo = phdr->p_vaddr;
    }

    if(lo < file_end) {
        uintptr_t hi = file_end;

        if(hi > page_end) {
            hi = page_end;
        }

        if(file_seek(file, phdr->p_offset + lo - phdr->p_vaddr, SEEK_SET) < 0) {
            return -EIO;
        }

        if(file_read(file, kva + lo - page, hi - lo, &file->offset) != (ssize_t)(hi - lo)) {
            return -EIO;
        }

        lo = hi;
    }

    if(mem_end > page_end) {
        mem_end = page_end;
    }

    if(lo < mem_end) {
        memset(kva + lo - page, 0, mem_end - lo);
    }

    return 0;
}

// Segments come sorted by address, and one may start in the page
// the previous one ends in; that page gets the rights of both
static int map_segment(vmspace_t* restrict vmspace, file_t* restrict file, const elf64_phdr_t* restrict phdr,
    uintptr_t* restrict mapped_end, unsigned* restrict mapped_vprot)
{
    unsigned vprot = phdr_vprot(phdr->p_flags);

    // The file is mapped in whole pages, so its offsets
    // and the addresses must share their place in a page
    if(page_offset(phdr->p_vaddr) != page_offset(phdr->p_offset)) {
        return -ENOEXEC;
    }

    uintptr_t seg_start = page_align(phdr->p_vaddr);
    uintptr_t seg_end = page_align_up(phdr->p_vaddr + phdr->p_memsz);
    uintptr_t file_end = page_align_up(phdr->p_vaddr + phdr->p_filesz);
    uintptr_t map_start = seg_start;

    if(seg_start < *mapped_end) {
        if(seg_start + PAGE_SIZE != *mapped_end) {
            return -ENOEXEC;
        }

        *mapped_vprot |= vprot;

        int retval = vms_protect(vmspace, virt_addr(seg_start), virt_addr(*mapped_end), *mapped_vprot);

        if(retval == 0) {
            retval = copy_page(vmspace, file, phdr, seg_start);
        }

        if(retval < 0) {
            return retval;
        }

        map_start = *mapped_end;
    }

    if(map_start < file_end) {
        vm_backing_t backing;
        backing.file = file;
        backing.file_offset = page_align(phdr->p_offset) + map_start - seg_start;
        backing.shared = false;
        backing.may_write = true;
        backing.device = false;

        int retval = vms_map(vmspace, virt_addr(map_start), virt_addr(file_end), vprot, 0, &backing);

        if(retval < 0) {
            return retval;
        }
    }

    uintptr_t anon_start = map_start;

    if(anon_start < file_end) {
        anon_start = file_end;
    }

    if(anon_start < seg_end) {
        int retval = vms_map(vmspace, virt_addr(anon_start), virt_addr(seg_end), vprot, 0, nullptr);

        if(retval < 0) {
            return retval;
        }
    }

    if(map_start < seg_end) {
        *mapped_end = seg_end;
        *mapped_vprot = vprot;
    }

    uintptr_t data_end = phdr->p_vaddr + phdr->p_filesz;

    if(phdr->p_memsz > phdr->p_filesz && page_offset(data_end) && page_align(data_end) >= map_start) {
        return copy_page(vmspace, file, phdr, page_align(data_end));
    }

    return 0;
}

static __always_inline bool ustack_reserve(uint8_t** restrict kva, uintptr_t* restrict uva, uintptr_t low_bound, size_t size)
{
    if(uva[0] - low_bound >= size) {
        kva[0] -= size;
        uva[0] -= size;
        return true;
    }

    return false;
}

static uintptr_t build_ustack(void* restrict top_kva, uintptr_t top_uva, size_t avail, const char** restrict argv,
    const char** restrict envp, const credentials_t* restrict credentials, const elf64_ehdr_t* restrict ehdr, uintptr_t phdr_uva)
{
    int argc = 0;

    while(argv && argv[argc]) {
        if(argc >= BINFMT_ARGS_MAX)
            return 0;
        argc += 1;
    }

    int envc = 0;

    while(envp && envp[envc]) {
        if(envc >= BINFMT_ARGS_MAX)
            return 0;
        envc += 1;
    }

    uint8_t* kva = top_kva;
    uintptr_t uva = top_uva;
    uintptr_t low_bound = top_uva - avail;

    uintptr_t argv_uva[BINFMT_ARGS_MAX];
    uintptr_t envp_uva[BINFMT_ARGS_MAX];

    for(int i = argc - 1; i >= 0; --i) {
        size_t len = strlen(argv[i]) + 1;

        if(!ustack_reserve(&kva, &uva, low_bound, len)) {
            return 0;
        }

        memcpy(kva, argv[i], len);
        argv_uva[i] = uva;
    }

    for(int i = envc - 1; i >= 0; --i) {
        size_t len = strlen(envp[i]) + 1;

        if(!ustack_reserve(&kva, &uva, low_bound, len)) {
            return 0;
        }

        memcpy(kva, envp[i], len);
        envp_uva[i] = uva;
    }

    if(!ustack_reserve(&kva, &uva, low_bound, uva % sizeof(uintptr_t))) {
        return 0;
    }

    elf64_auxv_t auxv[] = {
        { AT_PHDR, phdr_uva },
        { AT_PHENT, ehdr->e_phentsize },
        { AT_PHNUM, ehdr->e_phnum },
        { AT_ENTRY, ehdr->e_entry },
        { AT_PAGESZ, PAGE_SIZE },
        { AT_UID, credentials->uid },
        { AT_EUID, credentials->euid },
        { AT_GID, credentials->gid },
        { AT_EGID, credentials->egid },
        { AT_SECURE, cred_exec_secure(credentials) }, // Tells libc not to trust the environment
        { AT_NULL, 0 },
    };

    size_t auxv_bytes = sizeof(auxv);

    size_t fixed_size = auxv_bytes;
    fixed_size += sizeof(uintptr_t) * (envc + 1);
    fixed_size += sizeof(uintptr_t) * (argc + 1);
    fixed_size += sizeof(uintptr_t); // argc

    // The padding goes above the vector: libc finds
    // auxv right after the null that ends envp
    if((uva - fixed_size) % (2 * sizeof(uintptr_t))) {
        if(!ustack_reserve(&kva, &uva, low_bound, sizeof(uintptr_t))) {
            return 0;
        }
    }

    if(!ustack_reserve(&kva, &uva, low_bound, auxv_bytes)) {
        return 0;
    }

    memcpy(kva, auxv, auxv_bytes);

    if(!ustack_reserve(&kva, &uva, low_bound, sizeof(uintptr_t))) {
        return 0;
    }

    ((uintptr_t*)(kva))[0] = 0;

    for(int i = envc - 1; i >= 0; --i) {
        if(!ustack_reserve(&kva, &uva, low_bound, sizeof(uintptr_t))) {
            return 0;
        }

        ((uintptr_t*)(kva))[0] = envp_uva[i];
    }

    if(!ustack_reserve(&kva, &uva, low_bound, sizeof(uintptr_t))) {
        return 0;
    }

    ((uintptr_t*)(kva))[0] = 0;

    for(int i = argc - 1; i >= 0; --i) {
        if(!ustack_reserve(&kva, &uva, low_bound, sizeof(uintptr_t))) {
            return 0;
        }

        ((uintptr_t*)(kva))[0] = argv_uva[i];
    }

    if(!ustack_reserve(&kva, &uva, low_bound, sizeof(uintptr_t))) {
        return 0;
    }

    ((uintptr_t*)(kva))[0] = (uintptr_t)(argc);

    return uva;
}

static int elf64_probe(file_t* restrict file, const uint8_t* restrict header, size_t header_size)
{
    if(header_size < sizeof(elf64_ehdr_t))
        return -ENOEXEC;
    if(!ehdr_valid((const elf64_ehdr_t*)(header)))
        return -ENOEXEC;
    return 0;
}

static int elf64_setup(vmspace_t* restrict vmspace, file_t* restrict file, const char** restrict argv, const char** restrict envp,
    const credentials_t* restrict credentials, size_t stack_size, binfmt_entry_t* restrict out)
{
    elf64_ehdr_t ehdr;

    if(file_seek(file, 0, SEEK_SET) < 0) {
        return -EIO;
    }

    if(file_read(file, &ehdr, sizeof(ehdr), &file->offset) != (ssize_t)(sizeof(ehdr))) {
        return -EIO;
    }

    if(!ehdr_valid(&ehdr)) {
        return -ENOEXEC;
    }

    uintptr_t user_end = pmap_user_end(vmspace->pmap).value;

    // A kernel or non-canonical entry point
    // faults in kernel mode on the way to user mode
    if(ehdr.e_entry >= user_end) {
        return -ENOEXEC;
    }

    int retval;

    uintptr_t phdr_uva = 0;
    uintptr_t mapped_end = 0;
    unsigned mapped_vprot = VPROT_NONE;
    uint64_t phdrs_size = (uint64_t)(ehdr.e_phnum) * ehdr.e_phentsize;

    for(uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        elf64_phdr_t phdr;

        if(file_seek(file, ehdr.e_phoff + i * ehdr.e_phentsize, SEEK_SET) < 0) {
            retval = -EIO;
            goto error;
        }

        if(file_read(file, &phdr, sizeof(phdr), &file->offset) != (ssize_t)(sizeof(phdr))) {
            retval = -EIO;
            goto error;
        }

        if(phdr.p_type == PT_INTERP || phdr.p_type == PT_DYNAMIC) {
            retval = -ENOSYS;
            goto error;
        }

        if(phdr.p_type == PT_PHDR) {
            phdr_uva = phdr.p_vaddr;
        }

        if(phdr.p_type != PT_LOAD) {
            continue;
        }

        // Without PT_PHDR the headers are found through the segment that loads them
        if(phdr_uva == 0 && segment_loads_range(&phdr, ehdr.e_phoff, phdrs_size)) {
            phdr_uva = phdr.p_vaddr + (ehdr.e_phoff - phdr.p_offset);
        }

        if(phdr.p_filesz > phdr.p_memsz) {
            retval = -ENOEXEC;
            goto error;
        }

        // The kernel half's page tables are shared by every address space
        if(phdr.p_vaddr > user_end || phdr.p_memsz > user_end - phdr.p_vaddr) {
            retval = -ENOEXEC;
            goto error;
        }

        retval = map_segment(vmspace, file, &phdr, &mapped_end, &mapped_vprot);

        if(retval < 0) {
            goto error;
        }
    }

    retval = vms_map(vmspace, virt_addr(ELF64_STACK_TOP - stack_size), virt_addr(ELF64_STACK_TOP), VPROT_RW, 0, nullptr);

    if(retval < 0) {
        goto error;
    }

    // Arguments only get the topmost page, the rest is zeroed on first touch
    phys_addr_t args_phys = vms_touch(vmspace, virt_addr(ELF64_STACK_TOP - PAGE_SIZE));

    if(address_null(args_phys)) {
        retval = -ENOMEM;
        goto error;
    }

    uint8_t* args_top_kva = (uint8_t*)(phys_to_virt(args_phys)) + PAGE_SIZE;

    uintptr_t entry_rsp = build_ustack(args_top_kva, ELF64_STACK_TOP, PAGE_SIZE, argv, envp, credentials, &ehdr, phdr_uva);

    if(entry_rsp == 0) {
        retval = -E2BIG;
        goto error;
    }

    out->address = ehdr.e_entry;
    out->user_stack = entry_rsp;

    return 0;

error:
    vms_unmap_all(vmspace);
    return retval;
}

static const binfmt_ops_t elf64_ops = {
    .probe = &elf64_probe,
    .setup = &elf64_setup,
};

static binfmt_t elf64_binfmt = {
    .ops = &elf64_ops,
    .name = "elf64",
};

static void init_elf64(void)
{
    binfmt_add(&elf64_binfmt);
}

initcall_bsp_binfmt(init_elf64, INIT_MIDDLE);
