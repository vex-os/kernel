#include <vex/errno.h>
#include <vex/kmalloc.h>
#include <vex/serial_interface.h>
#include <vex/string.h>
#include <vex/strings.h>

#include "uart16550_internal.h"

#define UART_RBR 0 // Receiver Buffer, read
#define UART_THR 0 // Transmitter Holding, write
#define UART_DLL 0 // Divisor Latch, low byte, while LCR_DLAB
#define UART_IER 1 // Interrupt Enable
#define UART_DLM 1 // Divisor Latch, high byte, while LCR_DLAB
#define UART_IIR 2 // Interrupt Identification, read
#define UART_FCR 2 // FIFO Control, write
#define UART_LCR 3 // Line Control
#define UART_MCR 4 // Modem Control
#define UART_LSR 5 // Line Status
#define UART_MSR 6 // Modem Status
#define UART_SCR 7 // Scratch

#define IER_ERBFI 0x01 // Received data available
#define IER_ETBEI 0x02 // Transmitter holding register empty
#define IER_ELSI  0x04 // Receiver line status

#define IIR_NO_INTERRUPT 0x01
#define IIR_FIFOS        0xC0 // Both set while the FIFOs are enabled

#define FCR_ENABLE    0x01
#define FCR_CLEAR_RX  0x02
#define FCR_CLEAR_TX  0x04
#define FCR_TRIGGER_8 0x80 // Interrupt once 8 bytes are received

#define LCR_WORD_LENGTH 0x03 // Data bits less 5
#define LCR_STOP        0x04 // Two stop bits, or one and a half with 5 data bits
#define LCR_PARITY      0x08
#define LCR_EVEN        0x10
#define LCR_STICK       0x20 // The parity bit is fixed: zero with LCR_EVEN, one without
#define LCR_BREAK       0x40
#define LCR_DLAB        0x80 // Divisor Latch Access

#define MCR_DTR  0x01
#define MCR_RTS  0x02
#define MCR_OUT2 0x08 // Gates the interrupt line on the PC

#define LSR_DR   0x01 // Data Ready
#define LSR_OE   0x02 // Overrun Error
#define LSR_PE   0x04 // Parity Error
#define LSR_FE   0x08 // Framing Error
#define LSR_BI   0x10 // Break Interrupt
#define LSR_THRE 0x20 // Transmitter Holding Register Empty
#define LSR_TEMT 0x40 // Transmitter Empty, the shift register too

#define MSR_CTS 0x10
#define MSR_DSR 0x20
#define MSR_RI  0x40
#define MSR_DCD 0x80

#define FIFO_SIZE        16
#define RECEIVE_BATCH    64
#define INTERRUPT_ROUNDS 16 // Before letting a port that never goes quiet go

#define DIVISOR_MAX 0xFFFF

// Of LSR reads before a transmitter that never empties is given up on, as
// the console must not hang a panic; a byte at 300 bps is under 40000 of them
#define POLL_READS_MAX 1000000

typedef struct received_s received_t;

struct received_s {
    uint8_t bytes[RECEIVE_BATCH];
    uint8_t errors[RECEIVE_BATCH]; // SERIAL_ERROR_* of each byte
    size_t count;
};

static const uint8_t parity_bits[] = {
    [SERIAL_PARITY_NONE] = 0,
    [SERIAL_PARITY_ODD] = LCR_PARITY,
    [SERIAL_PARITY_EVEN] = LCR_PARITY | LCR_EVEN,
    [SERIAL_PARITY_MARK] = LCR_PARITY | LCR_STICK,
    [SERIAL_PARITY_SPACE] = LCR_PARITY | LCR_STICK | LCR_EVEN,
};

static uint8_t read_reg(const uart16550_t* restrict port, unsigned reg)
{
    return io_read8(&port->io, reg << port->reg_shift);
}

static void write_reg(const uart16550_t* restrict port, unsigned reg, uint8_t value)
{
    io_write8(&port->io, reg << port->reg_shift, value);
}

static uart16550_t* port_of(const serial_interface_t* restrict interface)
{
    return interface->ctxdata;
}

static unsigned parity_of(uint8_t lcr)
{
    for(unsigned parity = 0; parity < sizeof(parity_bits) / sizeof(parity_bits[0]); parity += 1) {
        if(parity_bits[parity] == (lcr & (LCR_PARITY | LCR_EVEN | LCR_STICK))) {
            return parity;
        }
    }

    // A stick bit without parity changes nothing on the line
    return SERIAL_PARITY_NONE;
}

static unsigned error_of(uint8_t lsr)
{
    if(lsr & LSR_BI) {
        return SERIAL_ERROR_BREAK;
    }

    if(lsr & LSR_FE) {
        return SERIAL_ERROR_FRAMING;
    }

    if(lsr & LSR_PE) {
        return SERIAL_ERROR_PARITY;
    }

    if(lsr & LSR_OE) {
        return SERIAL_ERROR_OVERRUN;
    }

    return SERIAL_ERROR_NONE;
}

static bool answers(const uart16550_t* restrict port)
{
    // Nothing decodes the registers of an absent port, so they read all ones
    if(read_reg(port, UART_LSR) == 0xFF) {
        return false;
    }

    write_reg(port, UART_SCR, 0x55);

    if(read_reg(port, UART_SCR) != 0x55) {
        return false;
    }

    write_reg(port, UART_SCR, 0xAA);

    return read_reg(port, UART_SCR) == 0xAA;
}

// A 16450 and older have no FIFO, and their IIR has the FIFO bits clear
static size_t detect_fifo(const uart16550_t* restrict port)
{
    write_reg(port, UART_FCR, FCR_ENABLE);

    if((read_reg(port, UART_IIR) & IIR_FIFOS) == IIR_FIFOS) {
        return FIFO_SIZE;
    }

    write_reg(port, UART_FCR, 0);

    return 1;
}

static void read_line(uart16550_t* restrict port, serial_line_t* restrict line)
{
    write_reg(port, UART_LCR, port->lcr | LCR_DLAB);
    unsigned divisor = read_reg(port, UART_DLL) | (unsigned)(read_reg(port, UART_DLM)) << 8;
    write_reg(port, UART_LCR, port->lcr);

    *line = (serial_line_t) {};

    if(divisor == 0) {
        return;
    }

    line->speed = port->clock / (16 * divisor);
    line->data_bits = (port->lcr & LCR_WORD_LENGTH) + 5;
    line->stop_bits = (port->lcr & LCR_STOP) ? 2 : 1;
    line->parity = parity_of(port->lcr);
}

// UNDONE: SERIAL_FLOW_RTSCTS, along with DCD, which a modem consumer needs
static int write_line(uart16550_t* restrict port, const serial_line_t* restrict line)
{
    uint32_t divisor = port->clock / (16 * line->speed);

    if(divisor == 0 || divisor > DIVISOR_MAX || line->flow) {
        return -EINVAL;
    }

    uint8_t lcr = (line->data_bits - 5) | parity_bits[line->parity] | (port->lcr & LCR_BREAK);

    if(line->stop_bits == 2) {
        lcr |= LCR_STOP;
    }

    write_reg(port, UART_LCR, lcr | LCR_DLAB);
    write_reg(port, UART_DLL, divisor & 0xFF);
    write_reg(port, UART_DLM, divisor >> 8);
    write_reg(port, UART_LCR, lcr);

    port->lcr = lcr;

    return 0;
}

static uint8_t drain_receiver(const uart16550_t* restrict port, received_t* restrict received)
{
    uint8_t lsr = read_reg(port, UART_LSR);

    while((lsr & LSR_DR) && received->count < RECEIVE_BATCH) {
        received->errors[received->count] = error_of(lsr);
        received->bytes[received->count] = read_reg(port, UART_RBR);
        received->count += 1;

        lsr = read_reg(port, UART_LSR);
    }

    return lsr;
}

static void deliver(uart16550_t* restrict port, const received_t* restrict received)
{
    size_t start = 0;

    for(size_t i = 0; i < received->count; ++i) {
        if(received->errors[i] == SERIAL_ERROR_NONE) {
            continue;
        }

        if(i > start) {
            serial_receive(&port->serial, &received->bytes[start], i - start);
        }

        serial_receive_error(&port->serial, received->bytes[i], received->errors[i]);
        start = i + 1;
    }

    if(received->count > start) {
        serial_receive(&port->serial, &received->bytes[start], received->count - start);
    }
}

// The bytes are taken without the lock, as the
// consumer's transmit_take takes the dispatch lock
static void transmit(uart16550_t* restrict port)
{
    uint8_t buffer[FIFO_SIZE];

    spinlock_acquire(&port->lock);
    port->tx_requested = false;
    spinlock_release(&port->lock);

    size_t count = serial_transmit_take(&port->serial, buffer, port->fifo_size);

    spinlock_acquire(&port->lock);

    for(size_t i = 0; i < count; ++i) {
        write_reg(port, UART_THR, buffer[i]);
    }

    if(count == 0 && !port->tx_requested) {
        port->ier &= ~IER_ETBEI;
        write_reg(port, UART_IER, port->ier);
    }

    spinlock_release(&port->lock);
}

// Reading IIR acknowledges a transmitter
// interrupt, and reading MSR a modem status one
static bool service(uart16550_t* restrict port)
{
    received_t received;
    received.count = 0;

    spinlock_acquire(&port->lock);

    if(read_reg(port, UART_IIR) & IIR_NO_INTERRUPT) {
        spinlock_release(&port->lock);
        return false;
    }

    uint8_t lsr = drain_receiver(port, &received);
    read_reg(port, UART_MSR);
    bool can_send = (lsr & LSR_THRE) && (port->ier & IER_ETBEI);

    spinlock_release(&port->lock);

    deliver(port, &received);

    if(can_send) {
        transmit(port);
    }

    return true;
}

static int uart16550_startup(serial_interface_t* restrict interface)
{
    uart16550_t* port = port_of(interface);
    bool intr_state = spinlock_acquire_intr(&port->lock);

    if(port->fifo_size > 1) {
        write_reg(port, UART_FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX | FCR_TRIGGER_8);
    }

    // What was pending before startup would interrupt at once
    read_reg(port, UART_LSR);
    read_reg(port, UART_RBR);
    read_reg(port, UART_IIR);
    read_reg(port, UART_MSR);

    port->mcr |= MCR_OUT2;
    write_reg(port, UART_MCR, port->mcr);

    // The transmitter interrupts at once while empty, and the handler takes what is waiting
    port->ier = IER_ERBFI | IER_ELSI | IER_ETBEI;
    write_reg(port, UART_IER, port->ier);

    spinlock_release_intr(&port->lock, intr_state);

    return 0;
}

static void uart16550_shutdown(serial_interface_t* restrict interface)
{
    uart16550_t* port = port_of(interface);
    bool intr_state = spinlock_acquire_intr(&port->lock);

    port->ier = 0;
    write_reg(port, UART_IER, port->ier);

    port->mcr &= ~MCR_OUT2;
    write_reg(port, UART_MCR, port->mcr);

    spinlock_release_intr(&port->lock, intr_state);
}

// The transmitter interrupts as IER_ETBEI goes from clear to set while THR is
// empty, so it is cleared first even when set, as an interrupt may be used up
static void uart16550_start_tx(serial_interface_t* restrict interface)
{
    uart16550_t* port = port_of(interface);
    bool intr_state = spinlock_acquire_intr(&port->lock);

    if(port->ier) {
        port->tx_requested = true;
        port->ier |= IER_ETBEI;
        write_reg(port, UART_IER, port->ier & ~IER_ETBEI);
        write_reg(port, UART_IER, port->ier);
    }

    spinlock_release_intr(&port->lock, intr_state);
}

static int uart16550_set_line(serial_interface_t* restrict interface, const serial_line_t* restrict line)
{
    uart16550_t* port = port_of(interface);
    bool intr_state = spinlock_acquire_intr(&port->lock);
    int retval = write_line(port, line);
    spinlock_release_intr(&port->lock, intr_state);

    return retval;
}

static void uart16550_set_modem(serial_interface_t* restrict interface, uint32_t lines)
{
    uart16550_t* port = port_of(interface);
    bool intr_state = spinlock_acquire_intr(&port->lock);

    port->mcr &= ~(MCR_DTR | MCR_RTS);

    if(lines & SERIAL_MODEM_DTR) {
        port->mcr |= MCR_DTR;
    }

    if(lines & SERIAL_MODEM_RTS) {
        port->mcr |= MCR_RTS;
    }

    write_reg(port, UART_MCR, port->mcr);

    spinlock_release_intr(&port->lock, intr_state);
}

static uint32_t uart16550_get_modem(serial_interface_t* restrict interface)
{
    uart16550_t* port = port_of(interface);
    bool intr_state = spinlock_acquire_intr(&port->lock);
    uint8_t msr = read_reg(port, UART_MSR);
    uint8_t mcr = port->mcr;
    spinlock_release_intr(&port->lock, intr_state);

    uint32_t lines = 0;
    lines |= (mcr & MCR_DTR) ? SERIAL_MODEM_DTR : 0;
    lines |= (mcr & MCR_RTS) ? SERIAL_MODEM_RTS : 0;
    lines |= (msr & MSR_DCD) ? SERIAL_MODEM_DCD : 0;
    lines |= (msr & MSR_DSR) ? SERIAL_MODEM_DSR : 0;
    lines |= (msr & MSR_CTS) ? SERIAL_MODEM_CTS : 0;
    lines |= (msr & MSR_RI) ? SERIAL_MODEM_RI : 0;

    return lines;
}

static void uart16550_set_break(serial_interface_t* restrict interface, bool on)
{
    uart16550_t* port = port_of(interface);
    bool intr_state = spinlock_acquire_intr(&port->lock);

    if(on) {
        port->lcr |= LCR_BREAK;
    }
    else {
        port->lcr &= ~LCR_BREAK;
    }

    write_reg(port, UART_LCR, port->lcr);

    spinlock_release_intr(&port->lock, intr_state);
}

static void uart16550_release(const serial_interface_t* restrict interface)
{
    kfree(port_of(interface));
}

static void wait_transmitter(const uart16550_t* restrict port, uint8_t bits)
{
    for(unsigned long i = 0; i < POLL_READS_MAX; ++i) {
        if((read_reg(port, UART_LSR) & bits) == bits) {
            return;
        }
    }
}

static void uart16550_poll_write(serial_interface_t* restrict interface, const void* restrict data, size_t size)
{
    uart16550_t* port = port_of(interface);
    const uint8_t* bytes = data;

    for(size_t i = 0; i < size; ++i) {
        wait_transmitter(port, LSR_THRE);
        write_reg(port, UART_THR, bytes[i]);
    }
}

static void uart16550_poll_drain(serial_interface_t* restrict interface)
{
    uart16550_t* port = port_of(interface);

    wait_transmitter(port, LSR_THRE | LSR_TEMT);
}

static void uart16550_poll_reset(serial_interface_t* restrict interface, const serial_line_t* restrict line)
{
    uart16550_t* port = port_of(interface);

    port->ier = 0;
    write_reg(port, UART_IER, port->ier);

    port->lcr &= ~LCR_BREAK;
    write_line(port, line);

    port->mcr = MCR_DTR | MCR_RTS;
    write_reg(port, UART_MCR, port->mcr);

    if(port->fifo_size > 1) {
        write_reg(port, UART_FCR, FCR_ENABLE);
    }
}

static const serial_interface_ops_t uart16550_ops = {
    .startup = &uart16550_startup,
    .shutdown = &uart16550_shutdown,
    .start_tx = &uart16550_start_tx,
    .set_line = &uart16550_set_line,
    .set_modem = &uart16550_set_modem,
    .get_modem = &uart16550_get_modem,
    .set_break = &uart16550_set_break,
    .release = &uart16550_release,
    .poll_write = &uart16550_poll_write,
    .poll_drain = &uart16550_poll_drain,
    .poll_reset = &uart16550_poll_reset,
};

uart16550_t* uart16550_create(device_t* restrict device, const io_region_t* restrict io, unsigned reg_shift, uint32_t clock,
    const char* restrict name)
{
    uart16550_t* port = kmalloc(sizeof(uart16550_t));

    if(port == nullptr) {
        return nullptr;
    }

    memset(port, 0, sizeof(uart16550_t));

    port->io = *io;
    port->reg_shift = reg_shift;
    port->clock = clock;
    port->fifo_size = 1;
    port->lock = SPINLOCK_INIT;
    port->serial.ctxdata = port;
    port->serial.char_interface.device = device;

    if(name) {
        kstrncpy(port->firmware_id, name, sizeof(port->firmware_id) - 1);

        port->serial.firmware_id = port->firmware_id;
    }

    return port;
}

int uart16550_add(uart16550_t* restrict port)
{
    if(!answers(port)) {
        return -ENODEV;
    }

    // The firmware may have left the interrupts on, or the line in loopback
    write_reg(port, UART_IER, 0);
    port->mcr = read_reg(port, UART_MCR) & (MCR_DTR | MCR_RTS);
    write_reg(port, UART_MCR, port->mcr);
    port->lcr = read_reg(port, UART_LCR) & ~(LCR_DLAB | LCR_BREAK);
    write_reg(port, UART_LCR, port->lcr);

    port->fifo_size = detect_fifo(port);

    serial_interface_t* serial = &port->serial;
    serial->ops = &uart16550_ops;
    serial->driver = "uart16550";
    serial->max_speed = port->clock / 16;
    serial->modem_lines = SERIAL_MODEM_DTR | SERIAL_MODEM_RTS | SERIAL_MODEM_DCD | SERIAL_MODEM_DSR | SERIAL_MODEM_CTS | SERIAL_MODEM_RI;

    read_line(port, &serial->line);

    // The serial core takes a zero speed as 38400 8N1, which the hardware has to be on then
    if(serial->line.speed == 0) {
        serial_line_t line = {};
        line.speed = SERIAL_DEFAULT_SPEED;
        line.data_bits = 8;
        line.stop_bits = 1;
        line.parity = SERIAL_PARITY_NONE;
        write_line(port, &line);
    }

    int retval = serial_interface_add(serial);

    if(retval < 0) {
        return retval;
    }

    port->added = true;

    return 0;
}

void uart16550_remove(uart16550_t* restrict port)
{
    char_interface_incref(&port->serial.char_interface);
    serial_interface_remove(&port->serial);
}

void uart16550_free(uart16550_t* restrict port)
{
    if(port->added) {
        char_interface_decref(&port->serial.char_interface);
        return;
    }

    kfree(port);
}

void uart16550_interrupt(uart16550_t* restrict port)
{
    for(unsigned round = 0; round < INTERRUPT_ROUNDS; round += 1) {
        if(!service(port)) {
            return;
        }
    }
}
