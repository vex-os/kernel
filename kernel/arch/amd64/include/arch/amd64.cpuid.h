#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

#define CPUID_1_ECX_XSAVE  UINT32_C(0x04000000)
#define CPUID_1_ECX_RDRAND UINT32_C(0x40000000)

#define CPUID_1_EDX_PAT  UINT32_C(0x00010000)
#define CPUID_1_EDX_FXSR UINT32_C(0x01000000)

#define CPUID_80000001_EDX_NX      UINT32_C(0x00100000)
#define CPUID_80000001_EDX_PDPE1GB UINT32_C(0x04000000)

#define CPUID_7_EBX_SMEP   UINT32_C(0x00000080)
#define CPUID_7_EBX_SMAP   UINT32_C(0x00100000)
#define CPUID_7_EBX_RDSEED UINT32_C(0x00040000)

static __always_inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* restrict eax, uint32_t* restrict ebx, uint32_t* restrict ecx,
    uint32_t* restrict edx)
{
    asm volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(leaf), "c"(subleaf));
}
