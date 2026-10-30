kernel_config_h = custom_target('generated.config.h',
    output: 'generated.config.h',
    command: [python, kconfig_gen_h,
        '--kconfig', kconfig_root,
        '--config', kconfig_file,
        # Meson rejects a file of the build directory as a dependency. The
        # load of .config reconfigures on a change, and the hash then changes
        # the command, so the header gets generated again
        '--config-hash', fs.hash(kconfig_file, 'md5'),
        '--output', '@OUTPUT@',
    ],
    env: kconfig_env,
    depend_files: kconfig_depends,
)

# Regenerated on every build so that UTSNAME_VERSION
# follows the checked out commit, see core/version.c
kernel_utsname_h = vcs_tag(
    input: 'utsname.h.in',
    output: 'generated.utsname.h',
    command: ['git', 'rev-parse', '--short', 'HEAD'],
    fallback: 'unknown',
)

kernel_includes = [include_directories('include', '.')]
kernel_link_args = []
kernel_link_depends = []

fs = import('fs')

# Meson takes -fPIE in a target's c_args as a mistake and warns for each archive:
# its pie keyword is only for executables, and its pic one gives -fPIC instead
add_project_arguments('-fPIE', language: 'c')

kernel_c_args = [
    # Every translation unit sees CONFIG_* values without having
    # to include anything, so a forgotten include can't silently
    # turn an option off. The path is relative to the build directory,
    # as Ninja names the generated header: an absolute one ends up in
    # depfiles as a file Ninja doesn't know it generates, and objects
    # only pick a changed .config up on the run after
    '-include', fs.relative_to(kernel_config_h.full_path(), meson.project_build_root()),
    '-D__kernel__',
    '-D__KERNEL__',
    '-DUTSNAME_SYSNAME="Vex"',
    '-DUTSNAME_RELEASE="@0@"'.format(meson.project_version()),
    '-DUTSNAME_MACHINE="@0@"'.format(arch),
    '-DUTSNAME_COMPILER="@0@"'.format(meson.get_compiler('c').get_id()),
    # Keeps __FILE__ relative to the source tree regardless
    # of where the build directory is
    '-fmacro-prefix-map=@0@/='.format(fs.relative_to(meson.project_source_root(), meson.project_build_root())),
    '-ffreestanding',
    '-fno-stack-protector',
    '-fno-stack-check',
    '-fno-lto',
    '-funsigned-char',
    '-Wno-unused-parameter',
    '-Wno-pointer-sign',
    '-Wno-sign-compare',
]

# Each top directory builds into its own archive, so that its sources
# get the kplog subsystem pr_* logs as. Its meson.build still only appends
# to kernel_sources, which starts over for every directory
kernel_subsystems = {
    'arch' / arch: 'KP_ARCH',
    'bus': 'KP_BUS',
    'core': 'KP_CORE',
    'crypto': 'KP_CRYPTO',
    'dev': 'KP_DEV',
    'device': 'KP_DEVICE',
    'drivers': 'KP_DRIVERS',
    'exec': 'KP_EXEC',
    'fbcons': 'KP_CORE',
    'firmware': 'KP_FIRMWARE',
    'fs': 'KP_FS',
    'init': 'KP_CORE',
    'ipc': 'KP_IPC',
    'irq': 'KP_IRQ',
    'libk': 'KP_CORE',
    'mm': 'KP_MM',
    'proc': 'KP_PROC',
    'random': 'KP_RANDOM',
    'time': 'KP_TIME',
}

kernel_includes += limine_includes
kernel_includes += uacpi_includes

kernel_libs = []

foreach directory, subsystem : kernel_subsystems
    kernel_sources = [kernel_config_h, kernel_utsname_h]
    generated_sources = kernel_sources.length()

    subdir(directory)

    # A directory that is switched off by its config adds nothing
    if kernel_sources.length() == generated_sources
        continue
    endif

    if directory == 'firmware'
        kernel_sources += uacpi_sources
    endif

    # Objects go into an archive which is then linked by calling the linker
    # directly: clang has no bare-metal driver for some targets (amd64 being
    # one of them) and would delegate the link to host's gcc otherwise
    kernel_libs += static_library('kernel_' + directory.replace('/', '_'), kernel_sources,
        include_directories: kernel_includes,
        c_args: kernel_c_args + ['-DKP_SUBSYSTEM=' + subsystem],
    )
endforeach

ld = find_program('ld')

kernel_elf = custom_target('vexsys.elf',
    input: kernel_libs,
    output: 'vexsys.elf',
    command: [ld,
        '-nostdlib',
        '-static',
        '-z', 'max-page-size=0x1000',
        '-z', 'text',
        kernel_link_args,
        '--whole-archive', '@INPUT@',
        '-o', '@OUTPUT@',
    ],
    depend_files: kernel_link_depends,
    build_by_default: true,
)
