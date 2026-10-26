#include <limine.h>
#include <vex/binfmt.h>
#include <vex/block_interface.h>
#include <vex/cmdline.h>
#include <vex/event.h>
#include <vex/file.h>
#include <vex/hart.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/mount.h>
#include <vex/panic.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/string.h>
#include <vex/version.h>

static volatile __used __section(".limine_requests") uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);
static volatile __used __section(".limine_requests_start") uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;
static volatile __used __section(".limine_requests_end") uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static void mount_root(void)
{
    const char* device_name = cmdline_argument(&sys_cmdline, "root");

    if(device_name == nullptr) {
        panic("kmain: no root device specified");
        unreachable();
    }

    const block_interface_t* interface = block_interface_find(device_name);

    if(interface == nullptr) {
        panic("kmain: %s not a block device", device_name);
        unreachable();
    }

    int retval = vfs_mount(nullptr, "/", interface, 0, nullptr);

    block_interface_decref(interface);

    if(retval < 0) {
        panic("kmain: %s: mount failed: %s", device_name, strerror(retval));
        unreachable();
    }
}

static void __noreturn init_watchdog(void* restrict arg)
{
    event_t* event = &init_exited;

    event_await_blocking(&event, 1);

    // FIXME: lets the disk image be inspected with mtools or
    // echfs-utils after a test init exits and the VM is killed;
    // drop it once there is a clean shutdown that syncs filesystems
    vfs_sync_all();

    panic("kmain: init process died");
    unreachable();
}

static void spawn_init(void)
{
    const char* path = cmdline_argument(&sys_cmdline, "init");

    if(path == nullptr) {
        path = CONFIG_DEFAULT_INIT;
    }

    process_t* process = process_create(nullptr);

    if(process == nullptr) {
        panic("kmain: out of memory");
        unreachable();
    }

    file_t* file;
    int retval = file_open(nullptr, path, O_RDONLY, 0, &kernel_credentials, &file);

    if(retval < 0) {
        panic("kmain: %s: file_open failed: %s", path, strerror(retval));
        unreachable();
    }

    const char* argv[] = { path, nullptr };
    const char* envp[] = { nullptr };

    binfmt_entry_t entry;
    retval = load_binary(process->vmspace, file, argv, envp, &kernel_credentials, &entry);

    file_close(file);

    if(retval < 0) {
        panic("kmain: %s: load_binary failed: %s", path, strerror(retval));
        unreachable();
    }

    thread_spawn_user(process, entry.address, entry.user_stack);
    thread_spawn_kernel(&init_watchdog, nullptr);
}

void __noreturn kmain(void)
{
    pr_emerg("%s %s %s", k_sysname, k_release, k_version);

    invoke_initcalls(initcalls_bsp_start, initcalls_bsp_end);

    pr_emerg("hanging around...");

    mount_root();

    spawn_init();

    enable_interrupts();

    for(;;) {
        hart_spin();
    }

    unreachable();
}
