#include <arch/amd64.cpuid.h>
#include <arch/cycles.h>

#define CPUID_0_EBX_INTEL UINT32_C(0x756E6547) // "Genu"
#define CPUID_0_EBX_AMD   UINT32_C(0x68747541) // "Auth"

#define CPUID_1_ECX_HYPERVISOR UINT32_C(0x80000000)

#define CPUID_80000007_EDX_INVTSC UINT32_C(0x00000100)

#define CPUID_HYPERVISOR_BASE     UINT32_C(0x40000000)
#define CPUID_HYPERVISOR_TIMING   UINT32_C(0x40000010) // EAX is the TSC frequency in kHz (VMware, QEMU)
#define CPUID_TSC_CRYSTAL         UINT32_C(0x15)
#define CPUID_PROCESSOR_FREQUENCY UINT32_C(0x16)

typedef struct cpu_signature_s cpu_signature_t;

struct cpu_signature_s {
    uint32_t vendor;
    uint32_t family;
    uint32_t model;
};

static void read_signature(cpu_signature_t* restrict signature, uint32_t vendor, uint32_t eax)
{
    signature->vendor = vendor;
    signature->family = (eax >> 8) & 0xF;
    signature->model = (eax >> 4) & 0xF;

    if(signature->family == 0xF) {
        signature->family += (eax >> 20) & 0xFF;
    }

    bool extended_model = signature->family == 0x6 || signature->family >= 0xF;

    if(extended_model) {
        signature->model |= ((eax >> 16) & 0xF) << 4;
    }
}

// The rule of CONSTANT_TSC in Linux: Intel from Core 2 and late Pentium 4, AMD from family 10h
static bool has_constant_rate(const cpu_signature_t* restrict signature)
{
    if(signature->vendor == CPUID_0_EBX_AMD) {
        return signature->family >= 0x10;
    }

    if(signature->vendor != CPUID_0_EBX_INTEL) {
        return false;
    }

    if(signature->family == 0x6) {
        return signature->model >= 0x0E;
    }

    if(signature->family == 0xF) {
        return signature->model >= 0x03;
    }

    return signature->family > 0x6;
}

static uint64_t hypervisor_frequency(void)
{
    uint32_t eax, ebx, ecx, edx;

    cpuid(CPUID_HYPERVISOR_BASE, 0, &eax, &ebx, &ecx, &edx);

    if(eax < CPUID_HYPERVISOR_TIMING) {
        return 0;
    }

    cpuid(CPUID_HYPERVISOR_TIMING, 0, &eax, &ebx, &ecx, &edx);
    return (uint64_t)(eax) * 1000;
}

static uint64_t native_frequency(uint32_t max_leaf)
{
    uint32_t eax, ebx, ecx, edx;

    if(max_leaf >= CPUID_TSC_CRYSTAL) {
        cpuid(CPUID_TSC_CRYSTAL, 0, &eax, &ebx, &ecx, &edx);

        // EBX/EAX is the ratio of the TSC to the crystal, and ECX is the crystal in Hz
        if(eax && ebx && ecx) {
            uint64_t crystal = ecx;
            return crystal * ebx / eax;
        }
    }

    // NOTE: the base frequency in MHz, which is the TSC frequency only
    // approximately; the calibration catches the difference
    if(max_leaf >= CPUID_PROCESSOR_FREQUENCY) {
        cpuid(CPUID_PROCESSOR_FREQUENCY, 0, &eax, &ebx, &ecx, &edx);
        return (uint64_t)(eax & 0xFFFF) * 1000000;
    }

    return 0;
}

void arch_cycles_probe(arch_cycles_info_t* restrict info)
{
    uint32_t eax, ebx, ecx, edx;

    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    uint32_t vendor = ebx;

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    bool hypervisor = ecx & CPUID_1_ECX_HYPERVISOR;

    cpu_signature_t signature = {};
    read_signature(&signature, vendor, eax);

    bool invariant = false;

    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);

    if(eax >= 0x80000007) {
        cpuid(0x80000007, 0, &eax, &ebx, &ecx, &edx);
        invariant = edx & CPUID_80000007_EDX_INVTSC;
    }

    // NOTE: QEMU, KVM and Hyper-V give a virtual TSC with
    // a constant rate, also where CPUID has no invtsc
    info->constant_rate = invariant || hypervisor || has_constant_rate(&signature);
    info->frequency_exact = false;
    info->frequency = 0;

    if(hypervisor) {
        info->frequency = hypervisor_frequency();
    }

    if(info->frequency == 0) {
        info->frequency = native_frequency(max_leaf);
    }
}
