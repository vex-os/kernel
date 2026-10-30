# The serial ports and the terminal of the test runner. The kernel command line in
# limine.conf and the runner see the same names. The shell port is the first
# one QEMU gets (stdio), and the log port the second one (a file); the ports
# are pinned to the COM ports of the q35 machine, as the probe order is not theirs
ktest_terminal = 'tty1337'
ktest_shell_port = 'serial1'
ktest_shell_device = 'acpi:PCI0/pci:0000:00:1f.0/isa:3f8'
ktest_log_port = 'serial2'
ktest_log_device = 'acpi:PCI0/pci:0000:00:1f.0/isa:2f8'

# Test programs, in the order that kst runs them with "all". A test that
# takes the machine over, such as the framebuffer one, goes last
ktest_programs = [
    'hello',
    'uname',
    'rlimit',
    'thread',
    'futex',
    'sleep',
    'sched',
    'affinity',
    'clock',
    'signal_threads',
    'process',
    'waitid',
    'sigsuspend',
    'sigtimedwait',
    'sigqueue',
    'itimer',
    'process_group',
    'job',
    'exec_threads',
    'tlb_shootdown',
    'mmap_flags',
    'mmap',
    'mlock',
    'commit',
    'shmem',
    'fs',
    'fs_times',
    'pread',
    'iov',
    'pipe',
    'poll',
    'getdents',
    'mount',
    'tmpfs',
    'fallocate',
    'fsize',
    'mremap',
    'path',
    'chroot',
    'flock',
    'fifo',
    'cputime',
    'kplog',
    'hwtree',
    'tty',
    'fbtest',
]

ktest_includes = include_directories(
    '.',
    '../kernel/include/usr',
    '../kernel/arch' / arch / 'include/usr',
)

# Userspace is freestanding too, and the project wide -fPIE of the
# kernel builds would otherwise reach these targets
ktest_c_args = [
    '-ffreestanding',
    '-fno-pic',
    '-fno-pie',
    '-fno-stack-protector',
    '-fno-stack-check',
    '-fno-lto',
    '-mgeneral-regs-only',
    '-mno-red-zone',
    '-Wno-unused-parameter',
    # Keeps __FILE__ relative to the source tree regardless of where the build directory is
    '-fmacro-prefix-map=@0@/='.format(fs.relative_to(meson.project_source_root(), meson.project_build_root())),
]

ktest_link_script = files('link.ld')
ktest_link_command = [
    ld,
    '-nostdlib',
    '-static',
    '-z', 'max-page-size=0x1000',
    '-z', 'noexecstack',
    '-T', ktest_link_script,
]

ktest_library = static_library('ktest', files('ktest.crt0.S', 'ktest.c'),
    include_directories: ktest_includes,
    c_args: ktest_c_args,
)

ktest_names = []

foreach program : ktest_programs
    ktest_names += '"' + program + '"'
endforeach

ktest_config_h = configure_file(
    output: 'generated.kst_tests.h',
    configuration: {
        'KST_TEST_NAMES': ', '.join(ktest_names),
        'KST_TERMINAL_PATH': '"/dev/@0@"'.format(ktest_terminal),
    },
)

# The test programs live in /kst on the DATA partition, the runner is at its root
ktest_targets = []
ktest_image_files = []

# Like the kernel, the runner sees what meson generated without including it. The
# path is relative to the build directory, as Ninja names the generated header
ktest_runner_library = static_library('ktest_runner', files('kst.c'),
    include_directories: ktest_includes,
    c_args: ktest_c_args + [
        '-include', fs.relative_to(meson.current_build_dir() / 'generated.kst_tests.h', meson.project_build_root()),
    ],
)

ktest_runner = custom_target('kst.elf',
    input: [ktest_library, ktest_runner_library],
    output: 'kst.elf',
    command: [ktest_link_command, '--whole-archive', '@INPUT@', '-o', '@OUTPUT@'],
    depend_files: ktest_link_script,
)

ktest_targets += ktest_runner
ktest_image_files += '@0@:/kst.elf'.format(ktest_runner.full_path())

foreach program : ktest_programs
    program_library = static_library('ktest_' + program, files('test.' + program + '.c'),
        include_directories: ktest_includes,
        c_args: ktest_c_args,
    )

    program_elf = custom_target(program + '.elf',
        input: [ktest_library, program_library],
        output: program + '.elf',
        command: [ktest_link_command, '--whole-archive', '@INPUT@', '-o', '@OUTPUT@'],
        depend_files: ktest_link_script,
    )

    ktest_targets += program_elf
    ktest_image_files += '@0@:/kst/@1@.elf'.format(program_elf.full_path(), program)
endforeach

limine_conf = configure_file(
    input: 'limine.conf.in',
    output: 'generated.limine.conf',
    configuration: {
        'KST_TERMINAL': ktest_terminal,
        'KST_SHELL_PORT': ktest_shell_port,
        'KST_SHELL_DEVICE': ktest_shell_device,
        'KST_LOG_PORT': ktest_log_port,
        'KST_LOG_DEVICE': ktest_log_device,
    },
)

ktest_image = custom_target('vexsys.boot.img',
    input: [
        limine_efi,
        limine_conf,
        kernel_elf,
    ],
    output: 'vexsys.boot.img',
    command: [find_program('sh'), files('mkimage.sh'), '@OUTPUT@', '@INPUT@', ktest_image_files],
    depends: ktest_targets,
    build_by_default: false,
)

alias_target('ktest-image', ktest_image)

run_target('ktest-run',
    command: [find_program('sh'), files('qemu.' + arch + '.sh')],
    env: {'VEX_IMAGE': ktest_image.full_path()},
    depends: ktest_image,
)
