kernel_includes += include_directories('include')

kernel_c_args += [
    '-m64',
    '-march=x86-64',
    '-mcmodel=kernel',
    '-mno-red-zone',
    '-mno-3dnow',
    '-mno-80387',
    '-mno-mmx',
    '-mno-sse',
    '-mno-sse2',
    '-DELF64_ARCH=EM_X86_64',
]

kernel_ldscript = meson.current_source_dir() / 'kernel.ld'
kernel_link_args += ['-m', 'elf_x86_64', '-T', kernel_ldscript]
kernel_link_depends += kernel_ldscript

subdir('bus')
subdir('core')
subdir('firmware')
subdir('irq')
subdir('mm')
subdir('proc')
subdir('random')
subdir('time')
