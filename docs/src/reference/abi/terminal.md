# Terminal

A terminal is a character device with a line discipline and a set of line
settings. A program reads and writes bytes, and the line discipline turns them
into lines, signals and echoes. A terminal has a carrier, such as a serial
port, but the carrier is separate: [Serial port](serial.md) describes it.

## The settings

The state of the line discipline is `struct __k_termios`:

| Field | Meaning |
| --- | --- |
| `c_iflag` | Input flags: parity, line breaks, flow control |
| `c_oflag` | Output flags: newline and tab processing |
| `c_cflag` | Control flags: data bits, stop bits, parity, `CLOCAL`, `CRTSCTS` |
| `c_lflag` | Local flags: `ICANON`, `ECHO`, `ISIG`, `IEXTEN` |
| `c_cc` | The 20 control characters |
| `c_ispeed`, `c_ospeed` | Speeds in bits per second |

An output speed of 0 hangs up the line, and an input speed of 0 follows the
output speed. The control characters have fixed indices in `c_cc`. `VINTR` is
at 0, `VQUIT` is at 1, `VERASE` is at 2 and `VSUSP` is at 9 for the signals
and the editing. `VMIN` is at 14 and `VTIME` is at 15 for a read in
noncanonical mode. `__k_POSIX_VDISABLE` turns one off.

## The requests

| Request | Argument | Meaning |
| --- | --- | --- |
| `__k_TCGETS` | `__k_termios` | Read the settings |
| `__k_TCSETS` | `__k_termios` | Set them now |
| `__k_TCSETSW` | `__k_termios` | Set them after the output drains |
| `__k_TCSETSF` | `__k_termios` | Set them after the output drains and the input is flushed |
| `__k_TIOCGWINSZ` | `__k_winsize` | Read the size of the window |
| `__k_TIOCSWINSZ` | `__k_winsize` | Set the size of the window |
| `__k_TIOCGPGRP` | `__k_pid_t` | Read the foreground process group |
| `__k_TIOCSPGRP` | `__k_pid_t` | Set the foreground process group |
| `__k_TIOCGSID` | `__k_pid_t` | Read the session id |
| `__k_TIOCSCTTY` | `__k_int32_t` | Make the terminal the controlling terminal of the caller |
| `__k_TIOCNOTTY` | None | Leave the controlling terminal |
| `__k_TCFLSH` | `__k_int32_t` | Flush the input, the output or both |
| `__k_TCXONC` | `__k_int32_t` | Stop or start the input or the output |
| `__k_TCDRAIN` | None | Wait until the output drains |
| `__k_TCSENDBREAK` | `__k_int32_t` | Send a break for that many milliseconds |
| `__k_TIOCEXCL` | None | Refuse later opens by other files |
| `__k_TIOCNXCL` | None | Allow them again |
| `__k_TIOCOUTQ` | `__k_int32_t` | Read the bytes still to send |

`__k_TIOCSCTTY` needs a session leader. A nonzero argument takes the terminal
from another session, and that needs `CAP_SYS_ADMIN`. The flush argument is one
of `__k_TCIFLUSH`, `__k_TCOFLUSH` and `__k_TCIOFLUSH`, and the flow argument is
one of `__k_TCOOFF`, `__k_TCOON`, `__k_TCIOFF` and `__k_TCION`.

## Limits

- The 20 control characters are fixed by `__k_NCCS`.
- On a terminal whose carrier is a serial port, `TCSETS` with a nonzero
  `c_ospeed` sets the line of the port. It sets the speed, the data bits, the
  stop bits, the parity and `CRTSCTS`. A zero `c_ospeed` hangs up and drops
  DTR.
- A request that the terminal does not know fails with `ENOTTY`. A break on a
  terminal whose carrier has no break succeeds and does nothing.

## Source

- [termios.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/termios.h#L1) — the terminal structure and the `'T'` requests
- [tty_interface.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/tty_interface.c) — the line discipline and the requests
- [tty_serial.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/tty_serial.c) — a terminal on a serial port
