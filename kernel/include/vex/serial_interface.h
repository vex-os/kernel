#pragma once

#include <vex/char_interface.h>
#include <vex/compiler.h>
#include <vex/console.h>
#include <vex/mutex.h>
#include <vex/serial.h>
#include <vex/spinlock.h>
#include <vex/types.h>

typedef struct dev_unit_s dev_unit_t;

#define SERIAL_ERROR_NONE    0U
#define SERIAL_ERROR_BREAK   1U
#define SERIAL_ERROR_PARITY  2U
#define SERIAL_ERROR_FRAMING 3U
#define SERIAL_ERROR_OVERRUN 4U // The hardware lost bytes before this one, which came intact

#define SERIAL_DEFAULT_SPEED 38400

typedef struct serial_interface_s serial_interface_t;
typedef struct serial_interface_ops_s serial_interface_ops_t;
typedef struct serial_interface_private_s serial_interface_private_t;
typedef struct serial_consumer_s serial_consumer_t;

struct serial_interface_ops_s {
    /// Enables the interrupts and starts taking input
    /// @pre The serial core serializes it with the other calls but start_tx, and the port is shut down
    /// @note nullptr when there is nothing to start
    int (*startup)(serial_interface_t* restrict interface);

    /// @pre The serial core serializes it with the other calls but start_tx, and the port is started up
    void (*shutdown)(serial_interface_t* restrict interface);

    /// There is output: take it with serial_transmit_take, now or from an interrupt
    /// @pre The dispatch lock of the port is not held
    /// @note Can be called again before the driver takes anything
    void (*start_tx)(serial_interface_t* restrict interface);

    /// @pre The serial core serializes it with the other calls but start_tx, and checked
    /// the line: a nonzero speed, 5 to 8 data bits, 1 or 2 stop bits, a known parity
    /// @retval -EINVAL The hardware can't run the line so
    /// @note Called with the port started up or not
    int (*set_line)(serial_interface_t* restrict interface, const serial_line_t* restrict line);

    /// @pre The serial core serializes it with the other calls but start_tx
    /// @param lines The SERIAL_MODEM_DTR and SERIAL_MODEM_RTS lines to raise. The driver lowers the other one
    /// @note nullptr when modem_lines has neither
    void (*set_modem)(serial_interface_t* restrict interface, uint32_t lines);

    /// @pre The serial core serializes it with the other calls but start_tx
    /// @return The SERIAL_MODEM_* lines that are raised
    /// @note nullptr when modem_lines has no input
    uint32_t (*get_modem)(serial_interface_t* restrict interface);

    /// @pre The serial core serializes it with the other calls but start_tx
    /// @note nullptr when the port can't send a break
    void (*set_break)(serial_interface_t* restrict interface, bool on);

    void (*release)(const serial_interface_t* restrict interface);

    /// Writes bytes for the kernel console, waiting on the transmitter instead of an interrupt
    /// @pre Interrupts may be disabled, and any other call may have stopped midway, as in a panic
    /// @note Takes no lock; nullptr when the port can't be a console
    void (*poll_write)(serial_interface_t* restrict interface, const void* restrict data, size_t size);

    /// Waits until what poll_write wrote is on the line
    /// @pre As for poll_write
    void (*poll_drain)(serial_interface_t* restrict interface);

    /// Turns the port's interrupts off and sets the line, so poll_write works whatever the driver left the port in
    /// @pre As for poll_write
    void (*poll_reset)(serial_interface_t* restrict interface, const serial_line_t* restrict line);
};

struct serial_interface_private_s {
    dev_unit_t* unit;
    mutex_t lock;                      // Serializes the calls into the driver but start_tx, and the binding
    spinlock_t dispatch_lock;          // Taken with interrupts disabled: consumer, which the bytes go to and come from
    const serial_consumer_t* consumer; // Bound, or an exchange's; changes under both locks, or the dispatch lock alone while unbinding
    serial_binding_t binding;          // consumer[0] is NUL while nothing is bound
    bool unbinding;                    // A consumer's unbind runs, without the lock
    bool started;
    uint32_t modem; // The SERIAL_MODEM_DTR and SERIAL_MODEM_RTS raised
    uint32_t lost;  // Bytes that came with nothing to take them
    bool removed;
    console_t console;          // Set up when console= names the port
    serial_line_t console_line; // What a panic puts the line back to
};

struct serial_interface_s {
    char_interface_t char_interface; // Its ops and ctxdata belong to the serial core
    serial_interface_private_t priv;
    const serial_interface_ops_t* ops;
    const char* driver;
    const char* firmware_id; // What the firmware calls the device; nullptr when it doesn't
    uint32_t max_speed;      // In bits per second
    uint32_t modem_lines;    // The SERIAL_MODEM_* lines the port has
    serial_line_t line;      // Hardware settings before serial_interface_add (zero speed: 38400 8N1), the serial core's after
    void* consumer_data;     // The bound consumer's own
    void* ctxdata;
};

struct serial_consumer_s {
    serial_consumer_t* next;
    const char* name;

    /// Makes the object that the consumer builds on the port, such as a terminal
    /// @pre The port's lock is held, and nothing is bound to the port
    /// @param index The index to give what it makes, SERIAL_ANY_INDEX for the first free; comes back as the one given
    /// @param object Zeroed; gets the name of what it made
    /// @retval -EEXIST The index is taken
    /// @retval -EINVAL The consumer doesn't take the options
    /// @note May set consumer_data, and gets the bytes from the port as soon as it returns
    int (*bind)(serial_interface_t* restrict interface, const char* restrict options, int32_t* restrict index, char* restrict object,
        size_t size);

    /// Removes the object that bind made
    /// @pre The lock of the port is not held
    /// @param force The port is going away, so it is taken away even when in use
    /// @retval -EBUSY It is in use, and force is false
    /// @note Calls serial_interface_detach before it frees what receive and transmit_take use
    /// The last calls from the object into the port can need the lock
    int (*unbind)(serial_interface_t* restrict interface, bool force);

    /// @pre The port's dispatch lock is held, so this may run in an interrupt handler
    void (*receive)(serial_interface_t* restrict interface, const uint8_t* restrict bytes, size_t count);

    /// @pre The port's dispatch lock is held, so this may run in an interrupt handler
    /// @param error One of SERIAL_ERROR_*, but SERIAL_ERROR_NONE
    void (*receive_error)(serial_interface_t* restrict interface, uint8_t byte, unsigned error);

    /// @pre The port's dispatch lock is held, so this may run in an interrupt handler
    /// @return How many bytes it put into the buffer
    size_t (*transmit_take)(serial_interface_t* restrict interface, void* restrict buffer, size_t size);
};

/// Makes the port's control node, serialN, and binds what the serial= command line option names for it
int serial_interface_add(serial_interface_t* restrict interface);

/// serial_interface_add that gives the port a number of its own, so that it does not take one from the real ports
/// @retval -EEXIST Another port has the number
int serial_interface_add_numbered(serial_interface_t* restrict interface, unsigned number);

/// Removes the object that is bound to the port, also when it is in use, and the control node
/// @post The serial core makes no more calls into the driver, except release
int serial_interface_remove(serial_interface_t* restrict interface);

/// @pre At initcall time, before the ports that the serial= option binds it to are added
void serial_consumer_register(serial_consumer_t* restrict consumer);

/// Hands bytes from the line to what the port is bound to; with nothing bound they are lost and counted
/// @note Any context, an interrupt handler among them
void serial_receive(serial_interface_t* restrict interface, const void* restrict bytes, size_t count);

/// Hands over a byte that came with a line error
/// @param byte What the hardware read; a break's is ignored
/// @param error One of SERIAL_ERROR_*, but SERIAL_ERROR_NONE
/// @note Any context, an interrupt handler among them
void serial_receive_error(serial_interface_t* restrict interface, uint8_t byte, unsigned error);

/// Takes bytes to send on the line
/// @return How many bytes it took; zero when there is nothing to send
/// @note Any context, an interrupt handler among them
size_t serial_transmit_take(serial_interface_t* restrict interface, void* restrict buffer, size_t size);

/// Stops the bytes that go to and come from the consumer of the port
/// @pre The bound consumer's unbind calls it
/// @post No receive or transmit_take of the consumer runs or starts
void serial_interface_detach(serial_interface_t* restrict interface);

/// Starts the port up for the bound consumer, such as on the first open of its terminal
/// @retval -ENODEV The port is removed
int serial_interface_startup(serial_interface_t* restrict interface);

void serial_interface_shutdown(serial_interface_t* restrict interface);

/// @retval -EINVAL The line is invalid, or the hardware can't run it
/// @retval -ENODEV The port is removed
int serial_interface_set_line(serial_interface_t* restrict interface, const serial_line_t* restrict line);

/// @param raise SERIAL_MODEM_DTR and SERIAL_MODEM_RTS to raise
/// @param lower SERIAL_MODEM_DTR and SERIAL_MODEM_RTS to lower, unless raise has them
void serial_interface_set_modem(serial_interface_t* restrict interface, uint32_t raise, uint32_t lower);

/// Sends a break and sleeps for as long as it lasts
/// @param msec Zero for a quarter of a second
/// @retval -EINVAL The port can't send a break
/// @retval -ENODEV The port is removed
int serial_interface_send_break(serial_interface_t* restrict interface, uint32_t msec);

/// Tells the driver to take the output
/// @pre The port's dispatch lock is not held
void serial_interface_start_tx(serial_interface_t* restrict interface);
