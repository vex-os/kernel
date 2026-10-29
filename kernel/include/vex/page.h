#pragma once

#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/limits.h>
#include <vex/types.h>

static __always_inline __nodiscard uintptr_t page_align_number(uintptr_t address)
{
    return align_floor(address, PAGE_SIZE);
}

static __always_inline __nodiscard uintptr_t page_align_up_number(uintptr_t address)
{
    return align_ceil(address, PAGE_SIZE);
}

static __always_inline __nodiscard size_t page_offset_number(uintptr_t address)
{
    return address & (PAGE_SIZE - 1);
}

static __always_inline __nodiscard void* page_align_ptr(void* restrict ptr)
{
    return (void*)(align_floor((uintptr_t)(ptr), PAGE_SIZE));
}

static __always_inline __nodiscard const void* page_align_const_ptr(const void* restrict ptr)
{
    return (const void*)(align_floor((uintptr_t)(ptr), PAGE_SIZE));
}

static __always_inline __nodiscard size_t page_count(size_t size)
{
    return align_ceil(size, PAGE_SIZE) / PAGE_SIZE;
}

#define DEFINE_PAGE_OPS(kind, type)                                                 \
    static __always_inline __nodiscard type kind##_addr_page_align(type address)    \
    {                                                                               \
        return kind##_addr(page_align_number(address.value));                       \
    }                                                                               \
                                                                                    \
    static __always_inline __nodiscard type kind##_addr_page_align_up(type address) \
    {                                                                               \
        return kind##_addr(page_align_up_number(address.value));                    \
    }                                                                               \
                                                                                    \
    static __always_inline __nodiscard size_t kind##_addr_page_offset(type address) \
    {                                                                               \
        return page_offset_number(address.value);                                   \
    }

DEFINE_PAGE_OPS(user, user_addr_t)
DEFINE_PAGE_OPS(virt, virt_addr_t)
DEFINE_PAGE_OPS(phys, phys_addr_t)

#define page_align(address)                \
    _Generic((address),                    \
        user_addr_t: user_addr_page_align, \
        virt_addr_t: virt_addr_page_align, \
        phys_addr_t: phys_addr_page_align, \
        default: page_align_number)((address))

#define page_align_up(address)                \
    _Generic((address),                       \
        user_addr_t: user_addr_page_align_up, \
        virt_addr_t: virt_addr_page_align_up, \
        phys_addr_t: phys_addr_page_align_up, \
        default: page_align_up_number)((address))

#define page_offset(address)                \
    _Generic((address),                     \
        user_addr_t: user_addr_page_offset, \
        virt_addr_t: virt_addr_page_offset, \
        phys_addr_t: phys_addr_page_offset, \
        default: page_offset_number)((address))
