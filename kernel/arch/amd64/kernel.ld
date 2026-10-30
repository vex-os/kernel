OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kmain)

PHDRS
{
    text    PT_LOAD FLAGS((1 << 0) | (1 << 2));
    rodata  PT_LOAD FLAGS((1 << 2));
    data    PT_LOAD FLAGS((1 << 1) | (1 << 2));
}

SECTIONS
{
    . = 0xFFFFFFFF80000000;
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    text_start = .;

    .text : {
        *(.text)
        *(.text.*)
    } :text
    
    text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    rodata_start = .;

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :rodata

    .initcalls : {
        initcalls_bsp_start = .;
        *(SORT(.initcalls.bsp.*))
        initcalls_bsp_end = .;

        initcalls_smp_start = .;
        *(SORT(.initcalls.smp.*))
        initcalls_smp_end = .;
    } :rodata

    .syscalls : {
        unsorted_syscalls_start = .;
        KEEP(*(.syscalls))
        unsorted_syscalls_end = .;
    } :rodata

    .drivers : {
        drivers_start = .;
        KEEP(*(.drivers))
        drivers_end = .;
    } :rodata

    rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    data_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :data

    .random_sources : {
        random_sources_start = .;
        KEEP(*(.random_sources))
        random_sources_end = .;
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data)
        *(.data.*)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :data

    data_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    kernel_end = .;

    /DISCARD/ : {
        *(.discard)
        *(.discard.*)
        *(.eh_frame*)
        *(.note.*)
        *(.note)
    }
}
