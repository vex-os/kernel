#pragma once

#include <vex/bitmap.h>
#include <vex/char_interface.h>
#include <vex/compiler.h>
#include <vex/event.h>
#include <vex/mutex.h>
#include <vex/page.h>
#include <vex/spinlock.h>
#include <vex/termios.h>
#include <vex/types.h>

typedef struct dev_unit_s dev_unit_t;
typedef struct process_s process_t;
typedef struct process_group_s process_group_t;
typedef struct session_s session_t;

#define TTY_RING_SIZE PAGE_SIZE

#define TTY_ERROR_NONE    0U
#define TTY_ERROR_BREAK   1U
#define TTY_ERROR_PARITY  2U
#define TTY_ERROR_FRAMING 3U
#define TTY_ERROR_OVERRUN 4U // The hardware lost bytes before this one, which came intact

typedef struct tty_ring_s tty_ring_t;
typedef struct tty_discipline_state_s tty_discipline_state_t;
typedef struct tty_interface_s tty_interface_t;
typedef struct tty_interface_ops_s tty_interface_ops_t;
typedef struct tty_interface_private_s tty_interface_private_t;

// The positions count up and wrap around
// as uint32_t, which TTY_RING_SIZE divides
struct tty_ring_s {
    uint8_t* data; // A page
    uint32_t head; // The next byte to take
    uint32_t tail; // Where the next byte goes
};

struct tty_discipline_state_s {
    tty_ring_t input;
    uint8_t delimiters[TTY_RING_SIZE / 8]; // A set bit ends a line at that input position; a NUL there is VEOF
    uint32_t line_end;                     // Input position past the last line read may hand out; the line being edited follows
    uint32_t line_column;                  // The output column the line being edited started at
    uint32_t column;                       // Of the output, for tabs, ONOCR and erasing
    bool literal_next;                     // VLNEXT came, so the next byte is plain data
    bool escaped;                          // XCASE: a backslash came, so the next letter is a capital
    bool input_stopped;                    // IXOFF sent VSTOP, and VSTART follows once a reader made room
    bool hardcopy_erasing;                 // ECHOPRT: erased bytes are being echoed between \ and /
};

struct tty_interface_ops_s {
    /// Sets the hardware up and starts taking input
    /// @pre The first open, and the device's console is released
    /// @post The tty core follows a success with set_termios, so the line needn't be configured here
    int (*open)(tty_interface_t* restrict interface);

    /// @pre The last close
    void (*close)(tty_interface_t* restrict interface);

    /// There is output: take it with tty_transmit_take, now or from an interrupt
    /// @pre No tty lock is held
    /// @note Can be called again before the driver takes anything
    void (*start_tx)(tty_interface_t* restrict interface);

    /// Applies c_cflag and the speeds to the hardware
    /// @pre The tty core serializes it with writes, and calls it right after open too
    /// @note nullptr when there is nothing to set
    int (*set_termios)(tty_interface_t* restrict interface, const termios_t* restrict termios);

    /// Requests the tty core doesn't know, TCSENDBREAK among them
    int (*ioctl)(tty_interface_t* restrict interface, uint32_t request, uintptr_t argument);

    void (*release)(const tty_interface_t* restrict interface);
};

struct tty_interface_private_s {
    spinlock_t lock;    // Taken with interrupts disabled: the rings, discipline and termios
    mutex_t write_lock; // Keeps writes from mixing, and termios from changing under one
    termios_t termios;  // The line discipline reads it, only the tty core changes it
    winsize_t winsize;
    tty_ring_t flip; // From the driver, for tty_worker
    tty_ring_t output;
    uint8_t xchar;       // VSTOP or VSTART to send ahead of the output; 0 for none, as that is _POSIX_VDISABLE
    bool output_stopped; // By VSTOP while IXON is set, or by TCOOFF
    uint32_t signals;    // Bit n set: signal n is to go to the foreground group
    uint32_t lost;       // Bytes the flip ring had no room for, and those the hardware overran
    bitmap_t flip_errors[TTY_RING_SIZE / BITMAP_CHUNK_BITS]; // Set bits mark TTY_ERROR at that flip position if it's between head and tail
    tty_discipline_state_t discipline;
    event_t readable;
    event_t writable;
    event_t drained;
    session_t* session;           // Under process_group_lock, a reference
    process_group_t* foreground;  // Under process_group_lock, a reference
    tty_interface_t* worker_next; // Under the worker queue's lock
    bool queued;                  // Under the worker queue's lock
    unsigned in_flight;           // Calls into the tty core that may still touch the device
    bool removed;
};

struct tty_interface_s {
    char_interface_t char_interface; // Its ops and ctxdata belong to the tty core
    tty_interface_private_t priv;
    const tty_interface_ops_t* ops;
    uint32_t initial_speed; // Of the line, in bits per second, until termios changes it; 0 for 38400
    void* ctxdata;
};

/// @param index The unit number in the name; DEV_NAME_NONE for a device that is its unit's only one
/// @retval -ENOMEM No memory for the rings
int tty_interface_add(tty_interface_t* restrict interface, dev_unit_t* restrict unit, unsigned index);

/// Hangs the terminal up: readers get end of file, writers -EIO, and its session loses it
/// @post The tty core makes no more calls into the driver, except release
int tty_interface_remove(tty_interface_t* restrict interface);

/// Takes the controlling terminal from the session that the exiting process leads, and sends SIGHUP and SIGCONT to its foreground
/// group, as POSIX has it. A process that leads no session or a session without a terminal is left alone
/// @pre The caller may sleep and does not hold process_group_lock
/// @note Files that are open on the terminal stay usable
void tty_interface_leader_exit(process_t* restrict process);

/// Hands bytes from the line to the tty core; what doesn't fit is lost and counted
/// @note Any context, an interrupt handler among them
void tty_receive(tty_interface_t* restrict interface, const void* restrict bytes, size_t count);

/// Hands over a byte that came with a line error, for termios to drop, mark or turn into SIGINT
/// @param byte What the hardware read; a break's is ignored
/// @note Any context, an interrupt handler among them
void tty_receive_error(tty_interface_t* restrict interface, uint8_t byte, unsigned error);

/// Takes bytes to send on the line
/// @return How many bytes it took; zero when the output is empty or stopped
/// @note Any context, an interrupt handler among them
size_t tty_transmit_take(tty_interface_t* restrict interface, void* restrict buffer, size_t size);

static __always_inline uint32_t tty_ring_used(const tty_ring_t* restrict ring)
{
    return ring->tail - ring->head;
}

static __always_inline uint32_t tty_ring_room(const tty_ring_t* restrict ring)
{
    return TTY_RING_SIZE - (ring->tail - ring->head);
}

static __always_inline uint8_t tty_ring_at(const tty_ring_t* restrict ring, uint32_t position)
{
    return ring->data[position % TTY_RING_SIZE];
}

static __always_inline void tty_ring_put(tty_ring_t* restrict ring, uint8_t byte)
{
    ring->data[ring->tail % TTY_RING_SIZE] = byte;
    ring->tail += 1;
}

static __always_inline uint8_t tty_ring_take(tty_ring_t* restrict ring)
{
    uint8_t byte = ring->data[ring->head % TTY_RING_SIZE];
    ring->head += 1;
    return byte;
}
