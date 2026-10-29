#ifndef USR_VEX_SERIAL_H
#define USR_VEX_SERIAL_H

#include <vex/ioctl.h>
#include <vex/types.h>

#define __k_SERIAL_PARITY_NONE  0
#define __k_SERIAL_PARITY_ODD   1
#define __k_SERIAL_PARITY_EVEN  2
#define __k_SERIAL_PARITY_MARK  3
#define __k_SERIAL_PARITY_SPACE 4

#define __k_SERIAL_FLOW_RTSCTS ((__k_uint8_t)(0x01))

/* DTR and RTS are the port's outputs, the others its inputs */
#define __k_SERIAL_MODEM_DTR ((__k_uint32_t)(0x01))
#define __k_SERIAL_MODEM_RTS ((__k_uint32_t)(0x02))
#define __k_SERIAL_MODEM_DCD ((__k_uint32_t)(0x04))
#define __k_SERIAL_MODEM_DSR ((__k_uint32_t)(0x08))
#define __k_SERIAL_MODEM_CTS ((__k_uint32_t)(0x10))
#define __k_SERIAL_MODEM_RI  ((__k_uint32_t)(0x20))

#define __k_SERIAL_EXCHANGE_MAX  256 /* Of what an exchange sends, and of what it reads back */
#define __k_SERIAL_NO_TERMINATOR (-1)

#define __k_SERIAL_ANY_INDEX (-1)

/* Reading needs only an open file; the requests that change the port need
 * the file opened with O_EXCL, which keeps everyone else from opening it,
 * and CAP_SYS_TTY_CONFIG */
#define __k_SERIAL_GET_INFO    __k_IOCTL_READ(sizeof(struct __k_serial_info), 'S', 0x01)
#define __k_SERIAL_GET_LINE    __k_IOCTL_READ(sizeof(struct __k_serial_line), 'S', 0x02)
#define __k_SERIAL_SET_LINE    __k_IOCTL_WRITE(sizeof(struct __k_serial_line), 'S', 0x03)
#define __k_SERIAL_GET_MODEM   __k_IOCTL_READ(sizeof(__k_uint32_t), 'S', 0x04)
#define __k_SERIAL_SET_MODEM   __k_IOCTL_WRITE(sizeof(struct __k_serial_modem), 'S', 0x05)
#define __k_SERIAL_BREAK       __k_IOCTL_WRITE(sizeof(__k_int32_t), 'S', 0x06)
#define __k_SERIAL_EXCHANGE    __k_IOCTL_RDWR(sizeof(struct __k_serial_exchange), 'S', 0x07)
#define __k_SERIAL_BIND        __k_IOCTL_RDWR(sizeof(struct __k_serial_binding), 'S', 0x08)
#define __k_SERIAL_UNBIND      __k_IOCTL_VOID('S', 0x09)
#define __k_SERIAL_GET_BINDING __k_IOCTL_READ(sizeof(struct __k_serial_binding), 'S', 0x0A)

struct __k_serial_info {
    char driver[32];
    char firmware_id[32];     /* What the firmware calls the device, such as an ACPI _HID; empty when it doesn't */
    __k_uint32_t max_speed;   /* In bits per second */
    __k_uint32_t modem_lines; /* The __k_SERIAL_MODEM_* lines the port has */
};

struct __k_serial_line {
    __k_uint32_t speed;    /* In bits per second */
    __k_uint8_t data_bits; /* 5 to 8 */
    __k_uint8_t stop_bits; /* 1 or 2 */
    __k_uint8_t parity;    /* __k_SERIAL_PARITY_* */
    __k_uint8_t flow;      /* __k_SERIAL_FLOW_* */
};

/* A line in both is raised */
struct __k_serial_modem {
    __k_uint32_t raise;
    __k_uint32_t lower;
};

/* Sends what send points to and reads the answer into what receive points to, until
 * receive_size bytes came, a byte equal to terminator came, or timeout_msec passed;
 * receive_size comes back as how many bytes were read. The port must be unbound */
struct __k_serial_exchange {
    __k_uintptr_t send;
    __k_uintptr_t receive;
    __k_uint32_t send_size;
    __k_uint32_t receive_size;
    __k_uint32_t timeout_msec;
    __k_int32_t terminator; /* A byte value, or __k_SERIAL_NO_TERMINATOR */
};

/* What makes use of the port: the consumer by name, such as "tty", its options,
 * and the index of what it makes, __k_SERIAL_ANY_INDEX for the first one free;
 * index and object come back as what was made, such as 0 and "tty0" */
struct __k_serial_binding {
    char consumer[16];
    char options[64];
    __k_int32_t index;
    char object[32];
};

#endif /* USR_VEX_SERIAL_H */
