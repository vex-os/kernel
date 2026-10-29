#pragma once

#include <vex/compiler.h>
#include <vex/file.h>
#include <vex/limits.h>
#include <vex/syscall.h>
#include <vex/uaccess.h>

// The count of a define_syscall* is the number of registers that the system call reads, descriptors and paths included.
// A handler gets the registers as parameters in their order, so the signature of the handler is the signature of the call.
// The parameters are integers as wide as a register, except where a macro hands over an object in their place:
//
//   define_syscall       (a, b, ...)         -> handler(a, b, ...)
//   define_syscall_fd    (fd, ...)           -> handler(file_t* file, ...)
//   define_syscall_dirfd (dirfd, path, ...)  -> handler(int dirfd, const char* path, ...)
//
// A new kind of macro is for what the dispatcher must do around the handler, and not for what a handler does with its arguments

#define SYSCALL_ENTRY(sc_name, sc_wrapper, sc_number)                            \
    static const __used __section(".syscalls") syscall_t __syscall_##sc_name = { \
        .function = &(sc_wrapper),                                               \
        .name = #sc_name,                                                        \
        .number = (sc_number),                                                   \
    }

#define SYSCALL_DEC_1 0
#define SYSCALL_DEC_2 1
#define SYSCALL_DEC_3 2
#define SYSCALL_DEC_4 3
#define SYSCALL_DEC_5 4
#define SYSCALL_DEC_6 5

// The leading arguments, if any, come before the ones read from args
#define SYSCALL_CALL_0(fn, args, ...) ((void)(args), (fn)(__VA_ARGS__))
#define SYSCALL_CALL_1(fn, args, ...) ((fn)(__VA_ARGS__ __VA_OPT__(, )(args)[0]))
#define SYSCALL_CALL_2(fn, args, ...) ((fn)(__VA_ARGS__ __VA_OPT__(, )(args)[0], (args)[1]))
#define SYSCALL_CALL_3(fn, args, ...) ((fn)(__VA_ARGS__ __VA_OPT__(, )(args)[0], (args)[1], (args)[2]))
#define SYSCALL_CALL_4(fn, args, ...) ((fn)(__VA_ARGS__ __VA_OPT__(, )(args)[0], (args)[1], (args)[2], (args)[3]))
#define SYSCALL_CALL_5(fn, args, ...) ((fn)(__VA_ARGS__ __VA_OPT__(, )(args)[0], (args)[1], (args)[2], (args)[3], (args)[4]))
#define SYSCALL_CALL_6(fn, args, ...) ((fn)(__VA_ARGS__ __VA_OPT__(, )(args)[0], (args)[1], (args)[2], (args)[3], (args)[4], (args)[5]))

#define define_syscall(sc_function, sc_number, sc_argc)                             \
    static intptr_t __syscall_wrapper_##sc_function(const uintptr_t* restrict args) \
    {                                                                               \
        return CONCAT(SYSCALL_CALL_, sc_argc)(sc_function, args);                   \
    }                                                                               \
    SYSCALL_ENTRY(sc_function, __syscall_wrapper_##sc_function, sc_number)

#define define_syscall_fd(sc_function, sc_number, sc_argc)                                          \
    static_assert((sc_argc) >= 1, "fd syscalls read at least the descriptor");                      \
    static intptr_t __syscall_fd_wrapper_##sc_function(const uintptr_t* restrict args)              \
    {                                                                                               \
        file_t* file;                                                                               \
        intptr_t retval = syscall_fd_get(args[0], &file);                                           \
        if(retval < 0) {                                                                            \
            return retval;                                                                          \
        }                                                                                           \
        retval = CONCAT(SYSCALL_CALL_, CONCAT(SYSCALL_DEC_, sc_argc))(sc_function, &args[1], file); \
        file_close(file);                                                                           \
        return retval;                                                                              \
    }                                                                                               \
    SYSCALL_ENTRY(sc_function, __syscall_fd_wrapper_##sc_function, sc_number)

#define define_syscall_dirfd(sc_function, sc_number, sc_argc)                                                                           \
    static_assert((sc_argc) >= 2, "dirfd syscalls read at least the descriptor and the path");                                          \
    static intptr_t __syscall_dirfd_wrapper_##sc_function(const uintptr_t* restrict args)                                               \
    {                                                                                                                                   \
        char path[PATH_MAX];                                                                                                            \
        ssize_t copied = user_copyin_str(path, args[1], sizeof(path));                                                                  \
        if(copied < 0) {                                                                                                                \
            return copied;                                                                                                              \
        }                                                                                                                               \
        return CONCAT(SYSCALL_CALL_, CONCAT(SYSCALL_DEC_, CONCAT(SYSCALL_DEC_, sc_argc)))(sc_function, &args[2], (int)(args[0]), path); \
    }                                                                                                                                   \
    SYSCALL_ENTRY(sc_function, __syscall_dirfd_wrapper_##sc_function, sc_number)
