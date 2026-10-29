#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

// The three kinds of address have the same operations. An address of one kind never takes the place of another:
// a number becomes an address through the constructor, and an address becomes a number through value

#define DEFINE_ADDRESS_OPS(kind, type)                                                   \
    static __always_inline __nodiscard type kind##_addr(uintptr_t value)                 \
    {                                                                                    \
        type result = {};                                                                \
        result.value = value;                                                            \
        return result;                                                                   \
    }                                                                                    \
                                                                                         \
    static __always_inline __nodiscard type kind##_addr_add(type address, size_t offset) \
    {                                                                                    \
        return kind##_addr(address.value + offset);                                      \
    }                                                                                    \
                                                                                         \
    static __always_inline __nodiscard type kind##_addr_sub(type address, size_t offset) \
    {                                                                                    \
        return kind##_addr(address.value - offset);                                      \
    }                                                                                    \
                                                                                         \
    /* The distance from low up to high; high is not below low */                        \
    static __always_inline __nodiscard size_t kind##_addr_diff(type high, type low)      \
    {                                                                                    \
        return high.value - low.value;                                                   \
    }                                                                                    \
                                                                                         \
    static __always_inline __nodiscard bool kind##_addr_equal(type left, type right)     \
    {                                                                                    \
        return left.value == right.value;                                                \
    }                                                                                    \
                                                                                         \
    static __always_inline __nodiscard bool kind##_addr_less(type left, type right)      \
    {                                                                                    \
        return left.value < right.value;                                                 \
    }                                                                                    \
                                                                                         \
    static __always_inline __nodiscard bool kind##_addr_null(type address)               \
    {                                                                                    \
        return address.value == 0;                                                       \
    }                                                                                    \
                                                                                         \
    static __always_inline __nodiscard bool kind##_addr_valid(type address)              \
    {                                                                                    \
        return address.value;                                                            \
    }

DEFINE_ADDRESS_OPS(user, user_addr_t)
DEFINE_ADDRESS_OPS(virt, virt_addr_t)
DEFINE_ADDRESS_OPS(phys, phys_addr_t)

#define address_add(address, offset) \
    _Generic((address), user_addr_t: user_addr_add, virt_addr_t: virt_addr_add, phys_addr_t: phys_addr_add)((address), (offset))

#define address_sub(address, offset) \
    _Generic((address), user_addr_t: user_addr_sub, virt_addr_t: virt_addr_sub, phys_addr_t: phys_addr_sub)((address), (offset))

#define address_diff(high, low) \
    _Generic((high), user_addr_t: user_addr_diff, virt_addr_t: virt_addr_diff, phys_addr_t: phys_addr_diff)((high), (low))

#define address_equal(left, right) \
    _Generic((left), user_addr_t: user_addr_equal, virt_addr_t: virt_addr_equal, phys_addr_t: phys_addr_equal)((left), (right))

#define address_less(left, right) \
    _Generic((left), user_addr_t: user_addr_less, virt_addr_t: virt_addr_less, phys_addr_t: phys_addr_less)((left), (right))

#define address_null(address) \
    _Generic((address), user_addr_t: user_addr_null, virt_addr_t: virt_addr_null, phys_addr_t: phys_addr_null)((address))

#define address_valid(address) \
    _Generic((address), user_addr_t: user_addr_valid, virt_addr_t: virt_addr_valid, phys_addr_t: phys_addr_valid)((address))

/// @note The pointer only names an address. Following the result of virt_addr_pointer is right
/// only for an address in the address space that is current, which is the kernel's for a pointer made here
static __always_inline __nodiscard virt_addr_t virt_addr_from_pointer(const volatile void* pointer)
{
    return virt_addr((uintptr_t)(pointer));
}

static __always_inline __nodiscard void* virt_addr_pointer(virt_addr_t address)
{
    return (void*)(address.value);
}
