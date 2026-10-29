#pragma once

#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/types.h>

extern virt_addr_t hhdm_base;

static __always_inline __nodiscard void* phys_to_virt(phys_addr_t phys)
{
    return (void*)(phys.value + hhdm_base.value);
}

static __always_inline __nodiscard phys_addr_t virt_to_phys(const void* virt)
{
    return phys_addr((uintptr_t)(virt) - (hhdm_base.value)); // clang-format antics
}
