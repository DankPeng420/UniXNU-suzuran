// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeAdreno430: the Xperia Z5 Compact's GPU (MSM8994, Adreno 430, a4xx), bring-up step 2b.
//
// The loader (v16) does what needs the RPM and TrustZone before XNU runs: the VDD_GFX corner and the
// RPM-owned gfx3d clock source, the OxiLi CX/GX power domains and branch clocks, the secure page-table
// pool and restore_sec_cfg(18) for the kgsl SMMU. This driver checks that state and, only if it is all
// there, repeats what the bare-metal gputest4 proved on the handset (2026-11-11):
//   1. A430 SP/TP out of collapse (RBBM_POWER_CNTL_IP), wait for RBBM_POWER_STATUS.SP_TP_PWR_ON,
//   2. the A430 branch of mainline a4xx_hw_init, without clock gating and with interrupts masked,
//   3. a 32 KiB ring in physically contiguous memory below 4 GiB (the SMMU context is left with
//      translation off by TrustZone, so the GPU's addresses are physical),
//   4. PM4 and PFP microcode, CP_ME_CNTL = 0, CP_ME_INIT,
//   5. two CP_MEM_WRITE packets into a target page, read back.
// It publishes no GPU service yet; the result is one log line. Boot-arg nativegpu=0 skips it.
// The kgsl SMMU's global registers (0xfdb10000) are never read: CR0 hangs the bus (gputest3).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

#include "A420Firmware.h"

namespace {

constexpr uint64_t kMmssPhys = 0xfd8c0000, kMmssSize = 0x10000;
constexpr uint64_t kGpuPhys = 0xfdb00000, kGpuSize = 0x40000;
constexpr uint32_t kGdscGx = 0x4024, kGfx3dCbcr = 0x4028, kGdscCx = 0x4034, kAhbCbcr = 0x403c, kTimerCbcr = 0x40b0;

// a4xx registers (freedreno a4xx.xml), dword indices
constexpr uint32_t RBBM_HW_VERSION = 0x0000, RBBM_CLOCK_CTL = 0x0020, RBBM_SP_HYST_CNT = 0x0021, RBBM_AHB_CTL0 = 0x0023,
    RBBM_AHB_CTL1 = 0x0024, RBBM_WAIT_IDLE_CLOCKS_CTL = 0x002b, RBBM_INTERFACE_HANG_INT_CTL = 0x002f,
    RBBM_INT_0_MASK = 0x0037, RBBM_RBBM_CTL = 0x003e, RBBM_CLOCK_CTL2 = 0x0042, RBBM_POWER_CNTL_IP = 0x0098,
    RBBM_PERFCTR_CTL = 0x0170, RBBM_GPU_BUSY_MASKED = 0x017a, RBBM_INT_0_STATUS = 0x017d, RBBM_AHB_ERROR_STATUS = 0x018f,
    RBBM_STATUS = 0x0191, RBBM_POWER_STATUS = 0x01b0, RBBM_WAIT_IDLE_CLOCKS_CTL2 = 0x01b8, CP_RB_BASE = 0x0200,
    CP_RB_CNTL = 0x0201, CP_RB_RPTR = 0x0204, CP_RB_WPTR = 0x0205, CP_PFP_UCODE_ADDR = 0x0223, CP_PFP_UCODE_DATA = 0x0224,
    CP_ME_RAM_WADDR = 0x0225, CP_ME_RAM_DATA = 0x0227, CP_ME_CNTL = 0x022d, CP_DEBUG = 0x022e, UCHE_TRAP_BASE_LO = 0x0e83,
    UCHE_TRAP_BASE_HI = 0x0e84, UCHE_CACHE_WAYS_VFD = 0x0e8c, VBIF_GATE_OFF_WRREQ_EN = 0x302a, VBIF_IN_RD_LIM_CONF0 = 0x302c,
    VBIF_IN_RD_LIM_CONF1 = 0x302d, VBIF_IN_WR_LIM_CONF0 = 0x3030, VBIF_IN_WR_LIM_CONF1 = 0x3031,
    VBIF_ROUND_ROBIN_QOS_ARB = 0x3049;
constexpr uint32_t kSpTpPwrOn = 1u << 20;
constexpr uint32_t kRingBytes = 0x8000, kRingDwords = kRingBytes / 4;
constexpr uint32_t kRbCntl = 0x0800020c;   // 32 KiB, 32-dword blocks, no RPTR shadow
constexpr uint32_t CP_ME_INIT = 0x48, CP_MEM_WRITE = 0x3d;
constexpr uint32_t pkt3(uint32_t opcode, uint32_t count) { return 0xc0000000u | ((count - 1) << 16) | (opcode << 8); }

struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint8_t *base = nullptr;
    bool open(uint64_t phys, uint64_t size)
    {
        IOMemoryDescriptor *range = IOMemoryDescriptor::withPhysicalAddress(phys, size, kIODirectionInOut);
        if (!range) return false;
        map = range->map(kIOMapInhibitCache);
        range->release();
        if (map) base = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
        return base != nullptr;
    }
    uint32_t read(uint32_t offset) const { return *reinterpret_cast<volatile uint32_t *>(base + offset); }
    void write(uint32_t offset, uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
};

Window mmss, gpu;
uint32_t gr(uint32_t index) { return gpu.read(index << 2); }
void gw(uint32_t index, uint32_t value) { gpu.write(index << 2, value); }

// The GPU reads and writes RAM behind the CPU caches: push our writes out, drop stale lines before a read.
void flushRange(void *start, size_t length)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t address = reinterpret_cast<uintptr_t>(start) & ~(line - 1);
    const uintptr_t end = reinterpret_cast<uintptr_t>(start) + length;
    for (; address < end; address += line) __asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

void checkpoint(const char *what)
{
    IOLog("NativeAdreno430: %s\n", what);
    IOSleep(2500);   // the console log reaches the eMMC once a second; a bus hang would otherwise hide this line
}

bool poweredByLoader()
{
    const uint32_t cx = mmss.read(kGdscCx), gx = mmss.read(kGdscGx), core = mmss.read(kGfx3dCbcr),
        ahb = mmss.read(kAhbCbcr), timer = mmss.read(kTimerCbcr);
    IOLog("NativeAdreno430: gdsc cx 0x%08x gx 0x%08x, cbcr gfx3d 0x%08x ahb 0x%08x rbbmtimer 0x%08x\n", cx, gx, core,
          ahb, timer);
    return (cx >> 31) && (gx >> 31) && !(core >> 31) && !(ahb >> 31) && (core & 1) && (ahb & 1);
}

void hwInit()
{
    gw(VBIF_GATE_OFF_WRREQ_EN, 0x00000001);
    gw(VBIF_IN_RD_LIM_CONF0, 0x18181818);
    gw(VBIF_IN_RD_LIM_CONF1, 0x00000018);
    gw(VBIF_IN_WR_LIM_CONF0, 0x18181818);
    gw(VBIF_IN_WR_LIM_CONF1, 0x00000018);
    gw(VBIF_ROUND_ROBIN_QOS_ARB, 0x00000003);
    IOLog("NativeAdreno430: after vbif: ahb error status 0x%08x\n", gr(RBBM_AHB_ERROR_STATUS));
    gw(RBBM_GPU_BUSY_MASKED, 0xffffffff);
    gw(RBBM_SP_HYST_CNT, 0x10);
    gw(RBBM_WAIT_IDLE_CLOCKS_CTL, 0x10);
    gw(RBBM_WAIT_IDLE_CLOCKS_CTL2, 0x30);
    gw(RBBM_AHB_CTL0, 0x00000001);
    gw(RBBM_AHB_CTL1, 0xa6ffffff);
    gw(RBBM_RBBM_CTL, 0x00000030);
    gw(RBBM_INTERFACE_HANG_INT_CTL, (1u << 30) | 0xffff);
    gw(RBBM_PERFCTR_CTL, 0x01);
    IOLog("NativeAdreno430: after rbbm: ahb error status 0x%08x\n", gr(RBBM_AHB_ERROR_STATUS));
    gw(UCHE_CACHE_WAYS_VFD, 0x07);
    gw(UCHE_TRAP_BASE_LO, 0xffff0000);
    gw(UCHE_TRAP_BASE_HI, 0xffff0000);
    IOLog("NativeAdreno430: after uche: ahb error status 0x%08x\n", gr(RBBM_AHB_ERROR_STATUS));
    gw(CP_DEBUG, (1u << 25) | (1u << 14));
    gw(RBBM_CLOCK_CTL, 0);    // hardware clock gating off for bring-up
    gw(RBBM_CLOCK_CTL2, 0);
    gw(RBBM_INT_0_MASK, 0);   // polled
    IOLog("NativeAdreno430: after cp/clock: ahb error status 0x%08x\n", gr(RBBM_AHB_ERROR_STATUS));
}

void bringUp()
{
    if (!mmss.open(kMmssPhys, kMmssSize)) {
        IOLog("NativeAdreno430: mmss window did not map\n");
        return;
    }
    checkpoint("reading the oxili power domains");
    if (!poweredByLoader()) {
        IOLog("NativeAdreno430: the loader did not power the GPU (needs loader v16); nothing touched\n");
        return;
    }
    if (!gpu.open(kGpuPhys, kGpuSize)) {
        IOLog("NativeAdreno430: kgsl window did not map\n");
        return;
    }
    checkpoint("first read of the kgsl window");
    IOLog("NativeAdreno430: hw version 0x%08x status 0x%08x power status 0x%08x me cntl 0x%08x ahb error 0x%08x\n",
          gr(RBBM_HW_VERSION), gr(RBBM_STATUS), gr(RBBM_POWER_STATUS), gr(CP_ME_CNTL), gr(RBBM_AHB_ERROR_STATUS));
    gw(RBBM_POWER_CNTL_IP, 0x778000);
    bool on = false;
    for (int i = 0; i < 20000 && !on; ++i) {
        on = (gr(RBBM_POWER_STATUS) & kSpTpPwrOn) != 0;
        if (!on) IODelay(5);
    }
    IOLog("NativeAdreno430: sp/tp %s (power status 0x%08x)\n", on ? "on" : "NOT on", gr(RBBM_POWER_STATUS));
    if (!on) return;

    // Ring and target: wired, contiguous, below 4 GiB (the GPU's addresses are 32-bit and physical).
    IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kRingBytes + 0x1000, 0xfffff000ULL);
    if (!memory) {
        IOLog("NativeAdreno430: no contiguous memory for the ring\n");
        return;
    }
    IOByteCount length = 0;
    const uint64_t phys = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    uint32_t *ring = static_cast<uint32_t *>(memory->getBytesNoCopy());
    if (!ring || !phys || length < kRingBytes + 0x1000 || (phys >> 32)) {
        IOLog("NativeAdreno430: ring memory unusable (phys 0x%llx, %llu contiguous bytes)\n", phys,
              static_cast<unsigned long long>(length));
        memory->release();
        return;
    }
    volatile uint32_t *target = ring + kRingDwords;
    const uint32_t targetPhys = static_cast<uint32_t>(phys) + kRingBytes;
    bzero(ring, kRingBytes + 0x1000);
    IOLog("NativeAdreno430: ring at phys 0x%08x, target 0x%08x\n", static_cast<uint32_t>(phys), targetPhys);

    hwInit();
    gw(CP_RB_CNTL, kRbCntl);
    gw(CP_RB_BASE, static_cast<uint32_t>(phys));
    gw(CP_ME_RAM_WADDR, 0);
    for (uint32_t i = 1; i < sizeof kA420Pm4 / 4; ++i) gw(CP_ME_RAM_DATA, kA420Pm4[i]);
    gw(CP_PFP_UCODE_ADDR, 0);
    for (uint32_t i = 1; i < sizeof kA420Pfp / 4; ++i) gw(CP_PFP_UCODE_DATA, kA420Pfp[i]);
    gw(CP_ME_CNTL, 0);
    IOLog("NativeAdreno430: microcode pm4 0x%08x pfp 0x%08x loaded, me cntl 0x%08x\n", kA420Pm4[1], kA420Pfp[5],
          gr(CP_ME_CNTL));

    static const uint32_t meInit[17] = {0x000003f7, 0, 0, 0, 0x00000080, 0x00000100, 0x00000180, 0x00006600, 0x00000150,
                                        0x0000014e, 0x00000154, 0x00000001, 0, 0, 0, 0, 0};
    uint32_t w = 0;
    ring[w++] = pkt3(CP_ME_INIT, 17);
    for (uint32_t value : meInit) ring[w++] = value;
    ring[w++] = pkt3(CP_MEM_WRITE, 2);
    ring[w++] = targetPhys;
    ring[w++] = 0xcafe430a;
    ring[w++] = pkt3(CP_MEM_WRITE, 2);
    ring[w++] = targetPhys + 4;
    ring[w++] = 0x12345678;
    flushRange(ring, kRingBytes + 0x1000);
    checkpoint("submitting CP_ME_INIT and two CP_MEM_WRITE");
    gw(CP_RB_WPTR, w);
    bool drained = false;
    for (int i = 0; i < 2000 && !drained; ++i) {
        drained = gr(CP_RB_RPTR) == w;
        if (!drained) IOSleep(1);
    }
    IOSleep(5);
    flushRange(const_cast<uint32_t *>(target), 0x1000);
    const uint32_t first = target[0], second = target[1];
    const bool pass = drained && first == 0xcafe430a && second == 0x12345678;
    IOLog("NativeAdreno430: rptr 0x%x wptr 0x%x target 0x%08x 0x%08x status 0x%08x int 0x%08x ahb error 0x%08x: %s\n",
          gr(CP_RB_RPTR), gr(CP_RB_WPTR), first, second, gr(RBBM_STATUS), gr(RBBM_INT_0_STATUS),
          gr(RBBM_AHB_ERROR_STATUS), pass ? "PASS, the GPU executes commands" : "FAIL");
    // The ring stays allocated: the command processor keeps its base.
}

void bringUpThread(void *, wait_result_t)
{
    IOSleep(90000);   // after NativeTouch and NativeCpuClock, with the home screen up
    bringUp();
    IOLog("NativeAdreno430: done\n");
}

}

class NativeAdreno430 final : public IOService {
    OSDeclareDefaultStructors(NativeAdreno430)
public:
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider)) return false;
        uint32_t enabled = 1;
        PE_parse_boot_argn("nativegpu", &enabled, sizeof enabled);
        if (!enabled) {
            IOLog("NativeAdreno430: skipped (nativegpu=0)\n");
            return true;
        }
        thread_t thread = nullptr;
        if (kernel_thread_start(bringUpThread, nullptr, &thread) != KERN_SUCCESS) {
            IOLog("NativeAdreno430: no bring-up thread\n");
            return true;
        }
        thread_deallocate(thread);
        IOLog("NativeAdreno430: bring-up in 90 s\n");
        return true;
    }
};
OSDefineMetaClassAndStructors(NativeAdreno430, IOService)
