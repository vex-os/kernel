#include <vex/char_interface.h>
#include <vex/dev_name.h>
#include <vex/fcntl.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/poll.h>
#include <vex/random.h>
#include <vex/string.h>

#include "random_internal.h"

// Both devices read the one generator
// and wait for its first seed, as on FreeBSD
static char_interface_t random_device;
static char_interface_t urandom_device;

static ssize_t random_dev_read(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset, void* restrict buffer,
    size_t size, unsigned flags)
{
    bool blocking = 0 == (flags & O_NONBLOCK);
    ssize_t waited = random_wait_ready(blocking);

    if(waited < 0) {
        return waited;
    }

    random_bytes_insecure(buffer, size);

    return size;
}

// Anyone can stir the pool, but only a source in the kernel earns credit
static ssize_t random_dev_write(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset,
    const void* restrict buffer, size_t size, unsigned flags)
{
    random_add(buffer, size, 0);

    return size;
}

static unsigned random_dev_poll(const char_interface_t* restrict interface, void* restrict file_ctxdata, poll_table_t* restrict table)
{
    poll_table_add(table, random_ready_event());

    if(random_ready()) {
        return POLL_READABLE | POLL_WRITABLE;
    }

    random_source_wake();

    return POLL_WRITABLE;
}

static const char_interface_ops_t random_dev_ops = {
    .read = &random_dev_read,
    .write = &random_dev_write,
    .poll = &random_dev_poll,
};

static void add_device(char_interface_t* restrict interface, dev_prefix_t* restrict prefix)
{
    interface->ops = &random_dev_ops;
    interface->device = nullptr;

    int retval = dev_prefix_claim(prefix);

    if(retval == 0) {
        retval = char_interface_add_unitless(interface, prefix);
    }

    if(retval < 0) {
        pr_warning("random: %s: %s", prefix->string, strerror(retval));
    }
}

static void init_random_devices(void)
{
    add_device(&random_device, &dev_prefix_random);
    add_device(&urandom_device, &dev_prefix_urandom);
}

initcall_bsp_dev(init_random_devices, INIT_MIDDLE);
