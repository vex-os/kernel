#include <vex/errno.h>
#include <vex/sprintf.h>
#include <vex/stdlib.h>
#include <vex/string.h>
#include <vex/strings.h>

const void* memchr(const void* restrict buf, int chr, size_t size)
{
    const unsigned char* sp = buf;

    while(size--) {
        if(*sp == chr) {
            return sp;
        }

        sp += 1;
    }

    return nullptr;
}

int memcmp(const void* restrict ba, const void* restrict bb, size_t size)
{
    const unsigned char* ap = ba;
    const unsigned char* bp = bb;

    while(size--) {
        if(*ap != *bp) {
            return *ap - *bp;
        }

        ap += 1;
        bp += 1;
    }

    return 0;
}

void* memcpy(void* restrict dst, const void* restrict src, size_t size)
{
    unsigned char* dp = dst;
    const unsigned char* sp = src;

    while(size--) {
        *dp++ = *sp++;
    }

    return dst;
}

void* memmove(void* dst, const void* src, size_t size)
{
    unsigned char* dp = dst;
    const unsigned char* sp = src;

    if(dp < sp) {
        while(size--) {
            *dp++ = *sp++;
        }
    }
    else if(dp > sp) {
        dp += size;
        sp += size;

        while(size--) {
            *--dp = *--sp;
        }
    }

    return dst;
}

void* memset(void* restrict dst, int chr, size_t size)
{
    unsigned char* sp = dst;

    while(size--) {
        *sp++ = chr;
    }

    return dst;
}

char* strcat(char* restrict dst, const char* restrict src)
{
    char* save = dst;
    if(*dst) {
        while(*(++dst)) {
            // empty
        }
    }

    while((*dst++ = *src++)) {
        // empty
    }

    return save;
}

const char* strchr(const char* restrict str, int chr)
{
    do {
        if(*str != chr) {
            continue;
        }

        return str;
    } while(*str++);

    return nullptr;
}

int strcmp(const char* restrict sa, const char* restrict sb)
{
    while(*sa && *sa == *sb) {
        sa += 1;
        sb += 1;
    }

    return *sa - *sb;
}

char* strcpy(char* restrict dst, const char* restrict src)
{
    char* save = dst;

    while((*dst++ = *src++)) {
        // empty
    }

    return save;
}

size_t strcspn(const char* restrict str, const char* restrict sym)
{
    size_t i = 0;

    while(*str) {
        if(strchr(sym, *str)) {
            return i;
        }

        str += 1;
        i += 1;
    }

    return i;
}

char* strerror(int errnum)
{
    static char buffer[128] = {};
    strerror_r(errnum, buffer, sizeof(buffer));
    return buffer;
}

int strerror_r(int errnum, char* restrict str, size_t size)
{
    switch(abs(errnum)) {
        case EDOM:
            kstrncpy(str, "argument out of domain of function", size);
            return 0;

        case EILSEQ:
            kstrncpy(str, "illegal byte sequence", size);
            return 0;

        case ERANGE:
            kstrncpy(str, "result too large", size);
            return 0;

        case E2BIG:
            kstrncpy(str, "argument list too long", size);
            return 0;

        case EACCES:
            kstrncpy(str, "permission denied", size);
            return 0;

        case EADDRINUSE:
            kstrncpy(str, "address in use", size);
            return 0;

        case EADDRNOTAVAIL:
            kstrncpy(str, "address not available", size);
            return 0;

        case EAFNOSUPPORT:
            kstrncpy(str, "address family not supported", size);
            return 0;

        case EAGAIN:
            kstrncpy(str, "resource temporarily unavailable", size);
            return 0;

        case EALREADY:
            kstrncpy(str, "connection already in progress", size);
            return 0;

        case EBADF:
            kstrncpy(str, "bad file descriptor", size);
            return 0;

        case EBADMSG:
            kstrncpy(str, "bad message", size);
            return 0;

        case EBUSY:
            kstrncpy(str, "device or resource busy", size);
            return 0;

        case ECANCELED:
            kstrncpy(str, "operation canceled", size);
            return 0;

        case ECHILD:
            kstrncpy(str, "no child processes", size);
            return 0;

        case ECONNABORTED:
            kstrncpy(str, "connection aborted", size);
            return 0;

        case ECONNREFUSED:
            kstrncpy(str, "connection refused", size);
            return 0;

        case ECONNRESET:
            kstrncpy(str, "connection reset", size);
            return 0;

        case EDEADLK:
            kstrncpy(str, "resource deadlock would occur", size);
            return 0;

        case EDESTADDRREQ:
            kstrncpy(str, "destination address required", size);
            return 0;

        case EDQUOT:
            kstrncpy(str, "disk quota exceeded", size);
            return 0;

        case EEXIST:
            kstrncpy(str, "file exists", size);
            return 0;

        case EFAULT:
            kstrncpy(str, "bad address", size);
            return 0;

        case EFBIG:
            kstrncpy(str, "file too large", size);
            return 0;

        case EHOSTUNREACH:
            kstrncpy(str, "host is unreachable", size);
            return 0;

        case EIDRM:
            kstrncpy(str, "identifier removed", size);
            return 0;

        case EINPROGRESS:
            kstrncpy(str, "operation in progress", size);
            return 0;

        case EINTR:
            kstrncpy(str, "interrupted function call", size);
            return 0;

        case EINVAL:
            kstrncpy(str, "invalid argument", size);
            return 0;

        case EIO:
            kstrncpy(str, "input/output error", size);
            return 0;

        case EISCONN:
            kstrncpy(str, "socket is connected", size);
            return 0;

        case EISDIR:
            kstrncpy(str, "is a directory", size);
            return 0;

        case ELOOP:
            kstrncpy(str, "too many levels of symbolic links", size);
            return 0;

        case EMFILE:
            kstrncpy(str, "too many open files", size);
            return 0;

        case EMLINK:
            kstrncpy(str, "too many links", size);
            return 0;

        case EMSGSIZE:
            kstrncpy(str, "message too long", size);
            return 0;

        case EMULTIHOP:
            kstrncpy(str, "multihop attempted", size);
            return 0;

        case ENAMETOOLONG:
            kstrncpy(str, "filename too long", size);
            return 0;

        case ENETDOWN:
            kstrncpy(str, "network is down", size);
            return 0;

        case ENETRESET:
            kstrncpy(str, "connection aborted by network", size);
            return 0;

        case ENETUNREACH:
            kstrncpy(str, "network unreachable", size);
            return 0;

        case ENFILE:
            kstrncpy(str, "too many files open in system", size);
            return 0;

        case ENOBUFS:
            kstrncpy(str, "no buffer space available", size);
            return 0;

        case ENODATA:
            kstrncpy(str, "no message is available on the stream head read queue", size);
            return 0;

        case ENODEV:
            kstrncpy(str, "no such device", size);
            return 0;

        case ENOENT:
            kstrncpy(str, "no such file or directory", size);
            return 0;

        case ENOEXEC:
            kstrncpy(str, "exec format error", size);
            return 0;

        case ENOLCK:
            kstrncpy(str, "no locks available", size);
            return 0;

        case ENOLINK:
            kstrncpy(str, "link has been severed", size);
            return 0;

        case ENOMEM:
            kstrncpy(str, "not enough space", size);
            return 0;

        case ENOMSG:
            kstrncpy(str, "no message of the desired type", size);
            return 0;

        case ENOPROTOOPT:
            kstrncpy(str, "protocol not available", size);
            return 0;

        case ENOSPC:
            kstrncpy(str, "no space left on device", size);
            return 0;

        case ENOSR:
            kstrncpy(str, "no stream resources", size);
            return 0;

        case ENOSTR:
            kstrncpy(str, "not a stream", size);
            return 0;

        case ENOSYS:
            kstrncpy(str, "function not implemented", size);
            return 0;

        case ENOTCONN:
            kstrncpy(str, "the socket is not connected", size);
            return 0;

        case ENOTDIR:
            kstrncpy(str, "not a directory", size);
            return 0;

        case ENOTEMPTY:
            kstrncpy(str, "directory not empty", size);
            return 0;

        case ENOTRECOVERABLE:
            kstrncpy(str, "state not recoverable", size);
            return 0;

        case ENOTSOCK:
            kstrncpy(str, "not a socket", size);
            return 0;

        case ENOTSUP:
            kstrncpy(str, "operation not supported", size);
            return 0;

        case ENOTTY:
            kstrncpy(str, "inappropriate i/o control operation", size);
            return 0;

        case ENXIO:
            kstrncpy(str, "no such device or address", size);
            return 0;

        case EOPNOTSUPP:
            kstrncpy(str, "operation not supported on socket", size);
            return 0;

        case EOVERFLOW:
            kstrncpy(str, "value too large to be stored in data type", size);
            return 0;

        case EOWNERDEAD:
            kstrncpy(str, "previous owner died", size);
            return 0;

        case EPERM:
            kstrncpy(str, "operation not permitted", size);
            return 0;

        case EPIPE:
            kstrncpy(str, "broken pipe", size);
            return 0;

        case EPROTO:
            kstrncpy(str, "protocol error", size);
            return 0;

        case EPROTONOSUPPORT:
            kstrncpy(str, "protocol not supported", size);
            return 0;

        case EPROTOTYPE:
            kstrncpy(str, "protocol wrong type for socket", size);
            return 0;

        case EROFS:
            kstrncpy(str, "read-only file system", size);
            return 0;

        case ESPIPE:
            kstrncpy(str, "invalid seek", size);
            return 0;

        case ESRCH:
            kstrncpy(str, "no such process", size);
            return 0;

        case ESTALE:
            kstrncpy(str, "stale file handle", size);
            return 0;

        case ETIME:
            kstrncpy(str, "stream ioctl() timeout", size);
            return 0;

        case ETIMEDOUT:
            kstrncpy(str, "connection timed out", size);
            return 0;

        case ETXTBSY:
            kstrncpy(str, "text file busy", size);
            return 0;

        case EXDEV:
            kstrncpy(str, "cross-device link", size);
            return 0;

        case 0:
            kstrncpy(str, "no error", size);
            return 0;

        default:
            snprintf(str, size, "unknown errnum (%d)", errnum);
            return -EINVAL;
    }
}

size_t strlen(const char* restrict str)
{
    size_t i;

    for(i = 0; *str++; ++i) {
        // empty
    }

    return i;
}

char* strncat(char* restrict dst, const char* restrict src, size_t size)
{
    char* save = dst;

    while(*dst) {
        dst += 1;
    }

    while(size && (*dst++ = *src++)) {
        size -= 1;
    }

    if(size == 0) {
        *dst = 0;
    }

    return save;
}

int strncmp(const char* restrict sa, const char* restrict sb, size_t size)
{
    while(size-- && *sa && *sa == *sb) {
        sa += 1;
        sb += 1;
    }

    if(size == SIZE_MAX) {
        return 0;
    }

    return *sa - *sb;
}

char* strncpy(char* restrict dst, const char* restrict src, size_t size)
{
    char* save = dst;

    while(size && (*dst++ = *src++)) {
        size -= 1;
    }

    while(size-- > 1) {
        *dst++ = 0;
    }

    return save;
}

size_t strnlen(const char* restrict str, size_t size)
{
    size_t i;

    for(i = 0; *str++ && i < size; ++i) {
        // empty
    }

    return i;
}

const char* strpbrk(const char* restrict str, const char* restrict sym)
{
    while(*str) {
        if(strchr(sym, *str)) {
            return str;
        }

        str += 1;
    }

    return nullptr;
}

const char* strrchr(const char* restrict str, int chr)
{
    const char* cp = nullptr;

    do {
        if(*str == chr) {
            cp = str;
        }
    } while(*str++);

    return cp;
}

size_t strspn(const char* restrict str, const char* restrict sym)
{
    size_t i = 0;

    while(*str && strchr(sym, *str++)) {
        i += 1;
    }

    return i;
}

const char* strstr(const char* restrict str, const char* restrict substr)
{
    const char* ap = str;
    const char* bp;

    while(*str) {
        bp = substr;

        while(*bp && (*ap == *bp)) {
            ap += 1;
            bp += 1;
        }

        if(!*bp) {
            return str;
        }

        ap = ++str;
    }

    return nullptr;
}
