#include <vex/char_interface.h>
#include <vex/dev_name.h>
#include <vex/errno.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/serial_interface.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/tty_interface.h>
#include <vex/uaccess.h>

typedef struct tty_serial_s tty_serial_t;

struct tty_serial_s {
    tty_interface_t tty;
    serial_interface_t* port; // A reference, through its control node
    dev_unit_t* unit;
    bool hangup_on_close; // HUPCL, kept from set_termios as close has no termios
    bool hung_up;         // A zero speed lowered DTR
};

static_assert(SERIAL_ERROR_BREAK == TTY_ERROR_BREAK && SERIAL_ERROR_PARITY == TTY_ERROR_PARITY);
static_assert(SERIAL_ERROR_FRAMING == TTY_ERROR_FRAMING && SERIAL_ERROR_OVERRUN == TTY_ERROR_OVERRUN);

static tty_serial_t* tty_serial_of(const tty_interface_t* restrict interface)
{
    return interface->ctxdata;
}

static int tty_serial_open(tty_interface_t* restrict interface)
{
    tty_serial_t* terminal = tty_serial_of(interface);
    int retval = serial_interface_startup(terminal->port);

    if(retval < 0) {
        return retval;
    }

    serial_interface_set_modem(terminal->port, SERIAL_MODEM_DTR | SERIAL_MODEM_RTS, 0);
    terminal->hung_up = false;

    return 0;
}

static void tty_serial_close(tty_interface_t* restrict interface)
{
    tty_serial_t* terminal = tty_serial_of(interface);

    if(terminal->hangup_on_close) {
        serial_interface_set_modem(terminal->port, 0, SERIAL_MODEM_DTR | SERIAL_MODEM_RTS);
    }

    serial_interface_shutdown(terminal->port);
}

static void tty_serial_start_tx(tty_interface_t* restrict interface)
{
    tty_serial_t* terminal = tty_serial_of(interface);
    serial_interface_start_tx(terminal->port);
}

static uint8_t data_bits_of(uint32_t cflag)
{
    switch(cflag & CSIZE) {
        case CS5:
            return 5;
        case CS6:
            return 6;
        case CS7:
            return 7;
        default:
            return 8;
    }
}

static uint8_t parity_of(uint32_t cflag)
{
    if(0 == (cflag & PARENB)) {
        return SERIAL_PARITY_NONE;
    }

    if(cflag & PARODD) {
        return SERIAL_PARITY_ODD;
    }

    return SERIAL_PARITY_EVEN;
}

static int tty_serial_set_termios(tty_interface_t* restrict interface, const termios_t* restrict termios)
{
    tty_serial_t* terminal = tty_serial_of(interface);
    uint32_t cflag = termios->c_cflag;

    if(termios->c_ispeed && termios->c_ispeed != termios->c_ospeed) {
        return -EINVAL;
    }

    terminal->hangup_on_close = cflag & HUPCL;

    if(termios->c_ospeed == 0) {
        serial_interface_set_modem(terminal->port, 0, SERIAL_MODEM_DTR);
        terminal->hung_up = true;
        return 0;
    }

    serial_line_t line = {};
    line.speed = termios->c_ospeed;
    line.data_bits = data_bits_of(cflag);
    line.stop_bits = (cflag & CSTOPB) ? 2 : 1;
    line.parity = parity_of(cflag);

    if(cflag & CRTSCTS) {
        line.flow = SERIAL_FLOW_RTSCTS;
    }

    int retval = serial_interface_set_line(terminal->port, &line);

    if(retval == 0 && terminal->hung_up) {
        serial_interface_set_modem(terminal->port, SERIAL_MODEM_DTR | SERIAL_MODEM_RTS, 0);
        terminal->hung_up = false;
    }

    return retval;
}

// A nonzero duration is in milliseconds, which POSIX leaves to the implementation
static int send_break(tty_serial_t* restrict terminal, uintptr_t argument)
{
    int32_t duration;

    if(user_copyin(&duration, argument, sizeof(duration)) < 0) {
        return -EFAULT;
    }

    if(duration < 0) {
        return -EINVAL;
    }

    return serial_interface_send_break(terminal->port, duration);
}

static int tty_serial_ioctl(tty_interface_t* restrict interface, uint32_t request, uintptr_t argument)
{
    if(request == TCSENDBREAK) {
        tty_serial_t* terminal = tty_serial_of(interface);

        return send_break(terminal, argument);
    }

    return -ENOTTY;
}

static void tty_serial_release(const tty_interface_t* restrict interface)
{
    tty_serial_t* terminal = tty_serial_of(interface);

    char_interface_decref(&terminal->port->char_interface);
    kfree(terminal);
}

static const tty_interface_ops_t tty_serial_ops = {
    .open = &tty_serial_open,
    .close = &tty_serial_close,
    .start_tx = &tty_serial_start_tx,
    .set_termios = &tty_serial_set_termios,
    .ioctl = &tty_serial_ioctl,
    .release = &tty_serial_release,
};

static int take_unit(int32_t index, dev_unit_t** restrict unit)
{
    if(index == SERIAL_ANY_INDEX) {
        return dev_unit_alloc(&dev_prefix_tty, nullptr, unit);
    }

    return dev_unit_claim(&dev_prefix_tty, index, unit);
}

static int tty_serial_bind(serial_interface_t* restrict interface, const char* restrict options, int32_t* restrict index,
    char* restrict object, size_t size)
{
    if(options[0]) {
        return -EINVAL;
    }

    tty_serial_t* terminal = kmalloc(sizeof(tty_serial_t));

    if(terminal == nullptr) {
        return -ENOMEM;
    }

    memset(terminal, 0, sizeof(*terminal));

    int retval = take_unit(*index, &terminal->unit);

    if(retval < 0) {
        kfree(terminal);
        return retval;
    }

    terminal->port = interface;
    terminal->tty.ops = &tty_serial_ops;
    terminal->tty.initial_speed = interface->line.speed;
    terminal->tty.ctxdata = terminal;

    char_interface_incref(&interface->char_interface);
    interface->consumer_data = terminal;

    retval = tty_interface_add(&terminal->tty, terminal->unit, DEV_NAME_NONE);

    if(retval < 0) {
        interface->consumer_data = nullptr;
        char_interface_decref(&interface->char_interface);
        dev_unit_free(terminal->unit);
        kfree(terminal);
        return retval;
    }

    *index = terminal->unit->number;
    kstrncpy(object, terminal->tty.char_interface.name, size - 1);

    return 0;
}

static int tty_serial_unbind(serial_interface_t* restrict interface, bool force)
{
    tty_serial_t* terminal = interface->consumer_data;
    dev_unit_t* unit = terminal->unit;

    if(!force) {
        int retval = char_interface_refuse_opens(&terminal->tty.char_interface);

        if(retval < 0) {
            return retval;
        }
    }

    // Before the removal, which may free the terminal
    serial_interface_detach(interface);

    // A hangup when forced; the terminal is freed once the last file of it closes
    tty_interface_remove(&terminal->tty);
    dev_unit_free(unit);

    return 0;
}

static void tty_serial_receive(serial_interface_t* restrict interface, const uint8_t* restrict bytes, size_t count)
{
    tty_serial_t* terminal = interface->consumer_data;
    tty_receive(&terminal->tty, bytes, count);
}

static void tty_serial_receive_error(serial_interface_t* restrict interface, uint8_t byte, unsigned error)
{
    tty_serial_t* terminal = interface->consumer_data;
    tty_receive_error(&terminal->tty, byte, error);
}

static size_t tty_serial_transmit_take(serial_interface_t* restrict interface, void* restrict buffer, size_t size)
{
    tty_serial_t* terminal = interface->consumer_data;
    return tty_transmit_take(&terminal->tty, buffer, size);
}

static serial_consumer_t tty_serial_consumer = {
    .name = "tty",
    .bind = &tty_serial_bind,
    .unbind = &tty_serial_unbind,
    .receive = &tty_serial_receive,
    .receive_error = &tty_serial_receive_error,
    .transmit_take = &tty_serial_transmit_take,
};

static void init_tty_serial(void)
{
    serial_consumer_register(&tty_serial_consumer);
}

initcall_bsp_dev(init_tty_serial, INIT_FIRST);
