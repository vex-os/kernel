#pragma once

typedef struct cpucaps_s cpucaps_t;

enum cpu_feature_e {
    CPU_FEATURE_FXSR,    // FPU FXSAVE/FXRSTOR
    CPU_FEATURE_PAT,     // Page Attribute Table
    CPU_FEATURE_NX,      // No-Execute bit in pmap entries
    CPU_FEATURE_PAGE_1G, // Very-large-sized pages
    CPU_FEATURE_XSAVE,   // FPU XSAVE/XRSTOR
    CPU_FEATURE_AVX,     // 256-bit vector math
    CPU_FEATURE_AVX512,  // 512-bit vector math
    CPU_FEATURE_SMEP,    // Supervisor Execution Prevention
    CPU_FEATURE_SMAP,    // Supervisor Access Prevention
    CPU_FEATURE_RDRAND,  // Random numbers from the on-chip generator
    CPU_FEATURE_RDSEED,  // Seeds from the on-chip entropy source
    NUM_CPU_FEATURES,
};

typedef enum cpu_feature_e cpu_feature_t;

void arch_cpucaps_probe(cpucaps_t* restrict caps);
