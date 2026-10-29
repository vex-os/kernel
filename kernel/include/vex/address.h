#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

static __always_inline __nodiscard user_addr_t user_addr(__k_uintptr_t value)
{
    user_addr_t result = {};
    result.value = value;
    return result;
}

static __always_inline __nodiscard virt_addr_t virt_addr(uintptr_t value)
{
    virt_addr_t result = {};
    result.value = value;
    return result;
}

static __always_inline __nodiscard phys_addr_t phys_addr(uintptr_t value)
{
    phys_addr_t result = {};
    result.value = value;
    return result;
}

static __always_inline __nodiscard user_addr_t user_addr_add(user_addr_t address, size_t offset)
{
    return user_addr(address.value + offset);
}
