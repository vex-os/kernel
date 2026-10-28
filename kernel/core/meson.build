kernel_sources += files(
    'cmdline.c',
    'console.c',
    'hostname.c',
    'kplog.c',
    'kprintf.c',
    'panic.c',
    'version.c',
)

if kconfig.get('CONFIG_KASSERT', 'n') == 'y'
    kernel_sources += files('assert.c')
endif
