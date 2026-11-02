# Serial port

A serial port is a character device that gives raw access to a line: the
speed, the framing, the modem signals and the bytes. It is not a terminal. A
terminal is one consumer that can take a port, and a port that carries a
packet protocol is another.

Reading a setting needs only an open file. A request that changes the port
needs the file opened with `O_EXCL`, which keeps every other open out, and
`CAP_SYS_TTY_CONFIG`. The rule lets a program dial a modem and then bind the
port without another program changing the line in between.

## The line

`__k_SERIAL_GET_INFO` fills a `struct __k_serial_info`:

| Field | Meaning |
| --- | --- |
| `driver` | The name of the driver, at most 32 bytes with the terminator |
| `firmware_id` | The id that the firmware gives the device, or empty |
| `max_speed` | The fastest speed in bits per second |
| `modem_lines` | The `__k_SERIAL_MODEM_*` lines that the port has |

`struct __k_serial_line` holds the speed in bits per second, 5 to 8 data bits
and 1 or 2 stop bits. It also holds a parity of `__k_SERIAL_PARITY_NONE`,
`ODD`, `EVEN`, `MARK` or `SPACE`, and a flow field with
`__k_SERIAL_FLOW_RTSCTS`.

The modem mask has `DTR` and `RTS` as outputs and `DCD`, `DSR`, `CTS` and `RI`
as inputs. `__k_SERIAL_SET_MODEM` takes one mask to raise and one to lower.

`__k_SERIAL_BREAK` sends a break for a number of milliseconds, at most 10000.

`__k_SERIAL_EXCHANGE` is the only request that moves bytes while no consumer
holds the port. It sends and receives at most 256 bytes each way, waits at
most 60000 ms, and stops at the terminator byte or at
`__k_SERIAL_NO_TERMINATOR`. The receive size comes back as the bytes that
arrived. The request fails with `EBUSY` while a consumer holds the port.

## Binding a consumer

The port itself has no read and no write. The bytes belong to a consumer that
takes the port with `__k_SERIAL_BIND`. The kernel keeps a registry of consumers
and the binding names one:

| Field | Meaning |
| --- | --- |
| `consumer` | The name of the consumer, at most 16 bytes with the terminator |
| `options` | An option string for that consumer, at most 64 bytes |
| `index` | The object to take, or `__k_SERIAL_ANY_INDEX` for the first free one |
| `object` | The name of the object that the consumer made, such as `tty0` |

The consumer decides what the bytes mean. The kernel registers `"tty"` today,
and that consumer makes a terminal on the port. A packet protocol such as
SLIP, PPP or KISS for AX.25 would be another consumer, with its own option
string and its own objects. The port ABI does not change for it: the program
sets the line, then binds the port to the consumer by name.

`__k_SERIAL_GET_BINDING` reads the binding back, or a zeroed one when nothing
is bound. `__k_SERIAL_UNBIND` fails with `ENXIO` when no consumer holds the
port, and with `EBUSY` when the object of the consumer is still open.

## Why there is no read and no write

An unbound port is a control device, and not an I/O device. It has no `read`
and no `write`. A program therefore cannot take the bytes of a port that a
service is about to bind. Two programs cannot talk over one line.

`__k_SERIAL_EXCHANGE` is the one exception. A service uses it to probe a
device before it assigns a consumer: it sends a command and reads the answer
under the exclusive open. The request needs the same `O_EXCL` and
`CAP_SYS_TTY_CONFIG` as every other change, and it fails with `EBUSY` while a
consumer holds the port.

After the bind, the consumer owns the bytes. A program then reads and writes
through the object of the consumer, such as a terminal. The port has no direct
path to the line any more.

## The requests

| Request | Argument | Meaning |
| --- | --- | --- |
| `__k_SERIAL_GET_INFO` | `__k_serial_info` | The driver, the firmware id, the fastest speed and the modem lines |
| `__k_SERIAL_GET_LINE` | `__k_serial_line` | The line settings |
| `__k_SERIAL_SET_LINE` | `__k_serial_line` | Set the speed, the data bits, the stop bits, the parity and the flow control |
| `__k_SERIAL_GET_MODEM` | `__k_uint32_t` | The state of the modem lines |
| `__k_SERIAL_SET_MODEM` | `__k_serial_modem` | Raise and lower the output lines |
| `__k_SERIAL_BREAK` | `__k_int32_t` | Send a break for that many milliseconds |
| `__k_SERIAL_EXCHANGE` | `__k_serial_exchange` | Send bytes and read the answer, with no consumer bound |
| `__k_SERIAL_BIND` | `__k_serial_binding` | Hand the port to a consumer |
| `__k_SERIAL_UNBIND` | None | Take it back |
| `__k_SERIAL_GET_BINDING` | `__k_serial_binding` | Read the binding, or a zeroed one when there is none |

## Limits

- An exchange sends and receives at most 256 bytes each way and waits at most
  60000 ms.
- A break lasts at most 10000 ms.
- The exchange uses the line settings of the port, and has no flow control of
  its own.
- A fixed field of a structure holds a string with a terminator, and the
  longest one is `options[64]`.

## Source

- [serial.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/serial.h#L1) — the structures and the `'S'` requests
- [serial_interface.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/serial_interface.c#L188) — bind, unbind and the requests
- [tty_serial.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/tty_serial.c#L169) — the `"tty"` consumer
