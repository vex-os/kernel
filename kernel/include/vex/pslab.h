#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

extern size_t pslab_numpages;
extern size_t pslab_numfree; // Read with an atomic load

uintptr_t pslab_alloc_phys(void) __nodiscard;
void* pslab_alloc_virt(void) __nodiscard;
void pslab_free_phys(uintptr_t phys);
void pslab_free_virt(void* restrict virt);
