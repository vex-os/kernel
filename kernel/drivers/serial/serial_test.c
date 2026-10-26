#include <vex/char_interface.h>
#include <vex/clocksrc.h>
#include <vex/dev_name.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/initcall.h>
#include <vex/ioctl.h>
#include <vex/kprintf.h>
#include <vex/serial_interface.h>
#include <vex/string.h>
#include <vex/uaccess.h>

// Far from the numbers of the real ports, which keep serial0, serial1 and so on
#define SERIAL_TEST_UNIT 1337

// Long enough for tty_worker to echo what was just written to the line
#define LINE_WAIT_NSEC UINT64_C(200000000)

// The argument points to an int32_t: a SERIAL_ERROR_* in bits 15:8, the byte in 7:0;
// only utest knows the number, so it isn't in the userspace headers
#define LINE_RECEIVE_ERROR IOCTL_WRITE(sizeof(int32_t), 't', 0x01)

#define TEST_MAX_SPEED 921600

static serial_interface_t test_port;
static char_interface_t test_line;
static event_t line_ready;
static uint32_t test_modem;

static void serial_test_start_tx(serial_interface_t* restrict interface)
{
    event_trigger(&line_ready, true);
}

static int serial_test_set_line(serial_interface_t* restrict interface, const serial_line_t* restrict line)
{
    return 0;
}

static void serial_test_set_modem(serial_interface_t* restrict interface, uint32_t lines)
{
    test_modem = lines;
}

// The far end is always there
static uint32_t serial_test_get_modem(serial_interface_t* restrict interface)
{
    return test_modem | SERIAL_MODEM_DCD | SERIAL_MODEM_DSR | SERIAL_MODEM_CTS;
}

static void serial_test_set_break(serial_interface_t* restrict interface, bool on)
{
    // empty
}

static const serial_interface_ops_t serial_test_ops = {
    .start_tx = &serial_test_start_tx,
    .set_line = &serial_test_set_line,
    .set_modem = &serial_test_set_modem,
    .get_modem = &serial_test_get_modem,
    .set_break = &serial_test_set_break,
};

static ssize_t test_line_read(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset, void* restrict buffer,
    size_t size, unsigned flags)
{
    uint64_t deadline = clock_source_now_ns() + LINE_WAIT_NSEC;

    for(;;) {
        size_t taken = serial_transmit_take(&test_port, buffer, size);
        uint64_t now = clock_source_now_ns();

        if(taken || now >= deadline) {
            return taken;
        }

        // A wakeup left over from an earlier start_tx only makes this look again
        event_t* ready = &line_ready;
        ssize_t result = event_await_timeout(&ready, 1, deadline - now);

        if(result < 0 && result != -ETIMEDOUT) {
            return result;
        }
    }
}

static ssize_t test_line_write(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset,
    const void* restrict buffer, size_t size, unsigned flags)
{
    serial_receive(&test_port, buffer, size);
    return size;
}

static int test_line_ioctl(const char_interface_t* restrict interface, void* restrict file_ctxdata, uint32_t request, uintptr_t argument)
{
    int32_t value;

    if(request != LINE_RECEIVE_ERROR) {
        return -ENOTTY;
    }

    if(user_copyin(&value, argument, sizeof(value)) < 0) {
        return -EFAULT;
    }

    unsigned error = (value >> 8) & 0xFF;

    if(error < SERIAL_ERROR_BREAK || error > SERIAL_ERROR_OVERRUN) {
        return -EINVAL;
    }

    serial_receive_error(&test_port, value & 0xFF, error);

    return 0;
}

static const char_interface_ops_t test_line_ops = {
    .read = &test_line_read,
    .write = &test_line_write,
    .ioctl = &test_line_ioctl,
};

static void init_serial_test(void)
{
    event_init(&line_ready);

    test_port.ops = &serial_test_ops;
    test_port.driver = "serial_test";
    test_port.max_speed = TEST_MAX_SPEED;
    test_port.modem_lines = SERIAL_MODEM_DTR | SERIAL_MODEM_RTS | SERIAL_MODEM_DCD | SERIAL_MODEM_DSR | SERIAL_MODEM_CTS;
    test_line.ops = &test_line_ops;

    int retval = serial_interface_add_numbered(&test_port, SERIAL_TEST_UNIT);

    if(retval == 0) {
        retval = dev_prefix_claim(&dev_prefix_serialtest);
    }

    if(retval == 0) {
        retval = char_interface_add_unitless(&test_line, &dev_prefix_serialtest);
    }

    if(retval < 0) {
        pr_warning("serial_test: %s", strerror(retval));
        return;
    }

    pr_inform("serial_test: %s, its far end %s", test_port.char_interface.name, test_line.name);
}

initcall_bsp_dev(init_serial_test, INIT_MIDDLE);
