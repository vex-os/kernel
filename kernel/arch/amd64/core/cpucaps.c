#include <arch/amd64.cpuid.h>
#include <vex/cpucaps.h>

#define XSTATE_AVX    UINT32_C(0x00000004)
#define XSTATE_AVX512 UINT32_C(0x000000E0) // Opmask, ZMM_Hi256 and Hi16_ZMM

const cpu_feature_desc_t arch_cpu_features[] = {
    [CPU_FEATURE_FXSR] = { .name = "fxsr", .scope = CPU_SCOPE_REQUIRED },
    [CPU_FEATURE_PAT] = { .name = "pat", .scope = CPU_SCOPE_REQUIRED },
    [CPU_FEATURE_NX] = { .name = "nx", .scope = CPU_SCOPE_REQUIRED },
    [CPU_FEATURE_PAGE_1G] = { .name = "pdpe1gb" },
    [CPU_FEATURE_XSAVE] = { .name = "xsave" },
    [CPU_FEATURE_AVX] = { .name = "avx" },
    [CPU_FEATURE_AVX512] = { .name = "avx512" },
    [CPU_FEATURE_SMEP] = { .name = "smep", .scope = CPU_SCOPE_LOCAL },
    [CPU_FEATURE_SMAP] = { .name = "smap", .scope = CPU_SCOPE_LOCAL },
    [CPU_FEATURE_RDRAND] = { .name = "rdrand", .scope = CPU_SCOPE_LOCAL },
    [CPU_FEATURE_RDSEED] = { .name = "rdseed", .scope = CPU_SCOPE_LOCAL },
};

static_assert(NUM_CPU_FEATURES == sizeof(arch_cpu_features) / sizeof(arch_cpu_features[0]));

void arch_cpucaps_probe(cpucaps_t* restrict caps)
{
    uint32_t eax, ebx, ecx, edx;

    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    bool has_xsave = ecx & CPUID_1_ECX_XSAVE;

    if(edx & CPUID_1_EDX_FXSR) {
        cpucaps_set(caps, CPU_FEATURE_FXSR);
    }

    if(edx & CPUID_1_EDX_PAT) {
        cpucaps_set(caps, CPU_FEATURE_PAT);
    }

    if(ecx & CPUID_1_ECX_RDRAND) {
        cpucaps_set(caps, CPU_FEATURE_RDRAND);
    }

    if(max_leaf >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);

        if(ebx & CPUID_7_EBX_SMEP) {
            cpucaps_set(caps, CPU_FEATURE_SMEP);
        }

        if(ebx & CPUID_7_EBX_SMAP) {
            cpucaps_set(caps, CPU_FEATURE_SMAP);
        }

        if(ebx & CPUID_7_EBX_RDSEED) {
            cpucaps_set(caps, CPU_FEATURE_RDSEED);
        }
    }

    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);

    if(eax >= 0x80000001) {
        cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);

        if(edx & CPUID_80000001_EDX_NX) {
            cpucaps_set(caps, CPU_FEATURE_NX);
        }

        if(edx & CPUID_80000001_EDX_PDPE1GB) {
            cpucaps_set(caps, CPU_FEATURE_PAGE_1G);
        }
    }

    if(has_xsave && max_leaf >= 0x0D) {
        cpucaps_set(caps, CPU_FEATURE_XSAVE);

        cpuid(0x0D, 0, &eax, &ebx, &ecx, &edx);

        if(eax & XSTATE_AVX) {
            cpucaps_set(caps, CPU_FEATURE_AVX);

            if(XSTATE_AVX512 == (eax & XSTATE_AVX512)) {
                cpucaps_set(caps, CPU_FEATURE_AVX512);
            }
        }
    }
}
