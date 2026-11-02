# Errors

An error is a positive number in `usr/vex/errno.h`. A system call
returns its negative in the result register, and the C library turns
that into `-1` with the number in `errno`.

The numbers are Vex numbers. They are not the Linux numbers. A program that
compares an error against a literal from Linux is wrong on Vex. A C library
defines each POSIX name from the `__k_` name in this header. It may add
numbers of its own above the kernel ones for errors that the kernel never
returns. The bootstrap C library, mlibc, does so from 2001.

ISO C takes the range 101 to 103. POSIX takes 201 and up. The kernel also has
`-ERESTART` (4095), which never reaches a program: the return path of a system
call turns it into a restart or into `-EINTR`. The C library has its own
`ERESTART` (2032) in the range that the kernel never returns, so the two do
not clash. The mechanism is in
[System calls](../../architecture/syscalls.md).

The string of an error comes from `strerror_r()` in `kernel/libk/string.c`. It
reads the absolute value of its argument, so a caller may pass the positive
number or the negated one from a system call. A number that it does not know
gives `unknown errnum (N)`, and 0 gives `no error`.

## The values

| Error | Value |
| --- | --- |
| `__k_EDOM` | 101 |
| `__k_EILSEQ` | 102 |
| `__k_ERANGE` | 103 |
| `__k_E2BIG` | 201 |
| `__k_EACCES` | 202 |
| `__k_EADDRINUSE` | 203 |
| `__k_EADDRNOTAVAIL` | 204 |
| `__k_EAFNOSUPPORT` | 205 |
| `__k_EAGAIN` | 206 |
| `__k_EALREADY` | 207 |
| `__k_EBADF` | 208 |
| `__k_EBADMSG` | 209 |
| `__k_EBUSY` | 210 |
| `__k_ECANCELED` | 211 |
| `__k_ECHILD` | 212 |
| `__k_ECONNABORTED` | 213 |
| `__k_ECONNREFUSED` | 214 |
| `__k_ECONNRESET` | 215 |
| `__k_EDEADLK` | 216 |
| `__k_EDESTADDRREQ` | 217 |
| `__k_EDQUOT` | 218 |
| `__k_EEXIST` | 219 |
| `__k_EFAULT` | 220 |
| `__k_EFBIG` | 221 |
| `__k_EHOSTUNREACH` | 222 |
| `__k_EIDRM` | 223 |
| `__k_EINPROGRESS` | 224 |
| `__k_EINTR` | 225 |
| `__k_EINVAL` | 226 |
| `__k_EIO` | 227 |
| `__k_EISCONN` | 228 |
| `__k_EISDIR` | 229 |
| `__k_ELOOP` | 230 |
| `__k_EMFILE` | 231 |
| `__k_EMLINK` | 232 |
| `__k_EMSGSIZE` | 233 |
| `__k_EMULTIHOP` | 234 |
| `__k_ENAMETOOLONG` | 235 |
| `__k_ENETDOWN` | 236 |
| `__k_ENETRESET` | 237 |
| `__k_ENETUNREACH` | 238 |
| `__k_ENFILE` | 239 |
| `__k_ENOBUFS` | 240 |
| `__k_ENODATA` | 241 |
| `__k_ENODEV` | 242 |
| `__k_ENOENT` | 243 |
| `__k_ENOEXEC` | 244 |
| `__k_ENOLCK` | 245 |
| `__k_ENOLINK` | 246 |
| `__k_ENOMEM` | 247 |
| `__k_ENOMSG` | 248 |
| `__k_ENOPROTOOPT` | 249 |
| `__k_ENOSPC` | 250 |
| `__k_ENOSR` | 251 |
| `__k_ENOSTR` | 252 |
| `__k_ENOSYS` | 253 |
| `__k_ENOTCONN` | 254 |
| `__k_ENOTDIR` | 255 |
| `__k_ENOTEMPTY` | 256 |
| `__k_ENOTSOCK` | 257 |
| `__k_ENOTSUP` | 258 |
| `__k_ENOTTY` | 259 |
| `__k_ENXIO` | 260 |
| `__k_EOPNOTSUPP` | 261 |
| `__k_EOVERFLOW` | 262 |
| `__k_EPERM` | 263 |
| `__k_EPIPE` | 264 |
| `__k_EPROTO` | 265 |
| `__k_EPROTONOSUPPORT` | 266 |
| `__k_EPROTOTYPE` | 267 |
| `__k_EROFS` | 268 |
| `__k_ESPIPE` | 269 |
| `__k_ESRCH` | 270 |
| `__k_ESTALE` | 271 |
| `__k_ETIME` | 272 |
| `__k_ETIMEDOUT` | 273 |
| `__k_ETXTBSY` | 274 |
| `__k_EXDEV` | 276 |
| `__k_ENOTRECOVERABLE` | 277 |
| `__k_EOWNERDEAD` | 278 |

A new error takes the next free number above the current ones. A number that
a program can see must not change.

## Source

- [errno.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/errno.h#L1) — the numbers
- [vex/errno.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/errno.h#L1) — the plain names for kernel code
- [string.c](https://github.com/vex-os/kernel/blob/main/kernel/libk/string.c#L157) — `strerror_r` and the string of each error
