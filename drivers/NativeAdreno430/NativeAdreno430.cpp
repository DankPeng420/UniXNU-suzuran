// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeAdreno430: the Xperia Z5 Compact's GPU (MSM8994, Adreno 430, a4xx).
//
// Bring-up (proven on the handset 2026-11-11, bare metal by gputest4 and here by the PASS line of boots 20/21):
// the loader (v16) does what needs the RPM and TrustZone before XNU runs -- the VDD_GFX corner and the RPM-owned
// gfx3d clock source, the OxiLi CX/GX power domains and branch clocks, the secure page-table pool and
// restore_sec_cfg(18) for the kgsl SMMU. This driver checks that state and only then:
//   1. takes the A430's SP/TP out of collapse (RBBM_POWER_CNTL_IP),
//   2. runs the A430 branch of mainline a4xx_hw_init, without clock gating and with interrupts masked,
//   3. gives the command processor a 32 KiB ring and the a420 PM4/PFP microcode, and CP_ME_INIT,
//   4. checks with two CP_MEM_WRITE that the GPU executes commands.
// The kgsl SMMU's context bank is left with translation off by TrustZone, so the GPU's addresses are physical:
// buffers are physically contiguous and below 4 GiB, and a buffer's GPU address ("iova") is its physical address.
// The SMMU's global registers (0xfdb10000) are never read: CR0 hangs the bus (gputest3).
//
// Then it publishes itself (registerService) for the user client, which is Linux's msm DRM interface as calls:
// selector = the ioctl's number, structure in and out as it is, a buffer's memory = clientMemoryForType(handle)
// (MsmUapi.h; HakiOS's hakios-drm shim makes freedreno's libdrm calls of these). Fences are the sequence numbers
// the command processor writes with CACHE_FLUSH_TS after each submission; waits poll them (no interrupt yet).
// Boot-arg nativegpu=0 skips everything.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/system.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#include "A420Firmware.h"
#include "MsmUapi.h"

// NativeSurface's (drivers/NativeDisplay): an IOSurface whose memory the GPU can address, held; null if none.
extern "C" OSObject *native_surface_gpu(uint32_t id, IOMemoryDescriptor **memory, uint8_t **kva, uint32_t *phys,
                                        uint64_t *size, uint32_t *offset, uint32_t *rowBytes, uint32_t *width,
                                        uint32_t *height, uint32_t *format, uint32_t *planes);

extern "C" uint64_t mach_continuous_time(void);
extern "C" void delay_for_interval_with_leeway(uint32_t interval, uint32_t leeway, uint32_t scale_factor);

namespace {

constexpr uint64_t kMmssPhys = 0xfd8c0000, kMmssSize = 0x10000;
constexpr uint64_t kGpuPhys = 0xfdb00000, kGpuSize = 0x40000;
constexpr uint32_t kGdscGx = 0x4024, kGfx3dCbcr = 0x4028, kGdscCx = 0x4034, kAhbCbcr = 0x403c, kTimerCbcr = 0x40b0;

// a4xx registers (freedreno a4xx.xml, adreno_common.xml), dword indices
constexpr uint32_t RBBM_HW_VERSION = 0x0000, RBBM_CLOCK_CTL = 0x0020, RBBM_SP_HYST_CNT = 0x0021, RBBM_AHB_CTL0 = 0x0023,
    RBBM_AHB_CTL1 = 0x0024, RBBM_WAIT_IDLE_CLOCKS_CTL = 0x002b, RBBM_INTERFACE_HANG_INT_CTL = 0x002f,
    RBBM_INT_0_MASK = 0x0037, RBBM_RBBM_CTL = 0x003e, RBBM_CLOCK_CTL2 = 0x0042, RBBM_POWER_CNTL_IP = 0x0098,
    RBBM_PERFCTR_CP_0_LO = 0x009c, RBBM_PERFCTR_CP_0_HI = 0x009d, RBBM_PERFCTR_CTL = 0x0170,
    RBBM_GPU_BUSY_MASKED = 0x017a, RBBM_INT_0_STATUS = 0x017d, RBBM_AHB_ERROR_STATUS = 0x018f, RBBM_STATUS = 0x0191,
    RBBM_POWER_STATUS = 0x01b0, RBBM_WAIT_IDLE_CLOCKS_CTL2 = 0x01b8, CP_RB_BASE = 0x0200, CP_RB_CNTL = 0x0201,
    CP_RB_RPTR = 0x0204, CP_RB_WPTR = 0x0205, CP_PFP_UCODE_ADDR = 0x0223, CP_PFP_UCODE_DATA = 0x0224,
    CP_ME_RAM_WADDR = 0x0225, CP_ME_RAM_DATA = 0x0227, CP_ME_CNTL = 0x022d, CP_DEBUG = 0x022e,
    CP_PERFCTR_CP_SEL_0 = 0x0500, CP_SCRATCH_REG2 = 0x057a, UCHE_TRAP_BASE_LO = 0x0e83, UCHE_TRAP_BASE_HI = 0x0e84,
    UCHE_CACHE_WAYS_VFD = 0x0e8c, VBIF_GATE_OFF_WRREQ_EN = 0x302a, VBIF_IN_RD_LIM_CONF0 = 0x302c,
    VBIF_IN_RD_LIM_CONF1 = 0x302d, VBIF_IN_WR_LIM_CONF0 = 0x3030, VBIF_IN_WR_LIM_CONF1 = 0x3031,
    VBIF_ROUND_ROBIN_QOS_ARB = 0x3049;
constexpr uint32_t kSpTpPwrOn = 1u << 20;
constexpr uint32_t kRingBytes = 0x8000, kRingDwords = kRingBytes / 4;
constexpr uint32_t kRbCntl = 0x0800020c;   // 32 KiB, 32-dword blocks, no RPTR shadow
// pm4 opcodes and events (adreno_pm4.xml)
constexpr uint32_t CP_WAIT_FOR_IDLE = 0x26, CP_MEM_WRITE = 0x3d, CP_INDIRECT_BUFFER_PFE = 0x3f, CP_EVENT_WRITE = 0x46,
    CP_ME_INIT = 0x48;
constexpr uint32_t CACHE_FLUSH_TS = 4, HLSQ_FLUSH = 7;
constexpr uint32_t pkt0(uint32_t reg, uint32_t count) { return ((count - 1) << 16) | (reg & 0x7fff); }
constexpr uint32_t pkt3(uint32_t opcode, uint32_t count) { return 0xc0000000u | ((count - 1) << 16) | (opcode << 8); }
constexpr uint32_t kPkt2 = 0x80000000u;

// What freedreno is told (MSM_PARAM_x). The A430 of the 8994 device tree is chip id 0x04030000 (qcom,chipid).
// GMEM is OCMEM on this SoC, which nothing powers yet: freedreno must render to system memory (FD_MESA_DEBUG=sysmem).
constexpr uint32_t kGpuId = 430, kChipId = 0x04030000, kGmemSize = 0x180000, kGmemBase = 0x100000;
constexpr uint32_t kMaxFreq = 300000000;

constexpr uint64_t kMaxBufferBytes = 64ull << 20, kMaxTotalBytes = 512ull << 20;
constexpr uint32_t kMaxHandles = 4096, kMaxSubmitBos = 4096, kMaxSubmitCmds = 128;
constexpr uint64_t kMaxWaitNs = 5000000000ull, kHangNs = 3000000000ull;

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

// --- the one command ring, its memory and the fences (all under gLock) ---
IOLock *gLock;
IOBufferMemoryDescriptor *gRingMemory;
volatile uint32_t *gRing;          // kRingDwords
volatile uint32_t *gStore;         // the page after the ring: [0] fence, [2..3] self-test
uint32_t gRingPhys, gStorePhys;
uint32_t gWptr;                    // dwords
uint32_t gSubmitted;               // last fence handed out
bool gReady, gHung;
uint64_t gTotalBytes;
uint64_t gSubmits;
// wait statistics (all clients; logged when a client closes)
uint64_t gWaits, gWaitNs, gWaitMaxNs;

// --- the buffer pool ---
// The GPU's addresses are physical, so every buffer is contiguous. Late in a boot the kernel rarely has 4 MB of it
// (boot 51: backboardd's first 750x1334 texture, "no contiguous memory for a 4194304-byte buffer", 140 s after power-on,
// and the window server could not draw). So memory is taken at start, while it is still in large pieces, and buffers
// are cut from it, a page at a time; a buffer that does not fit is allocated on its own as before.
// Boot-arg adrenopool=MB (default 192 with suzurangl=1, else 0: the route that needs it), taken in pieces of 64 MB, or
// 32 or 16 where 64 are not had (boot 52: not even 96 in one piece at 33 s, and 48 MB ran out once the lock screen
// drew). The window server's screen surfaces come from it too (native_gpu_memory(): NativeSurface), so the GPU draws
// into them itself.
constexpr unsigned kPoolChunks = 16;
constexpr uint32_t kChunkShift = 20;     // a buffer's place in the pool: chunk << 20 | first page
struct PoolChunk {
    IOBufferMemoryDescriptor *memory;
    uint8_t *kva;
    uint32_t phys, pages;
    uint8_t *map;                  // a byte a page: 1 = used
};
IOLock *gPoolLock;
PoolChunk gPool[kPoolChunks];
unsigned gPoolCount;
uint32_t gPoolPages, gPoolUsed;

void poolInit()
{
    uint32_t mb = 0, gl = 0;
    PE_parse_boot_argn("suzurangl", &gl, sizeof gl);
    if (gl == 1) mb = 192;
    PE_parse_boot_argn("adrenopool", &mb, sizeof mb);
    if (mb == 0 || (gPoolLock = IOLockAlloc()) == nullptr) return;
    uint32_t got = 0, piece = 64;
    while (got < mb && gPoolCount < kPoolChunks && piece >= 16) {
        const uint32_t want = mb - got < piece ? (mb - got < 16 ? 16 : mb - got) : piece;
        const uint64_t bytes = static_cast<uint64_t>(want) << 20;
        IOBufferMemoryDescriptor *pool = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
            bytes, 0xfffff000ULL);
        IOByteCount length = 0;
        const uint64_t phys = pool ? pool->getPhysicalSegment(0, &length, kIOMemoryMapperNone) : 0;
        uint8_t *map = nullptr;
        if (pool && phys && length >= bytes && !(phys >> 32) && pool->getBytesNoCopy() &&
            (map = IONewZero(uint8_t, bytes >> 12)) != nullptr) {
            PoolChunk &c = gPool[gPoolCount++];
            c.memory = pool;
            c.kva = static_cast<uint8_t *>(pool->getBytesNoCopy());
            c.phys = static_cast<uint32_t>(phys);
            c.pages = static_cast<uint32_t>(bytes >> 12);
            c.map = map;
            gPoolPages += c.pages;
            got += want;
            IOLog("NativeAdreno430: buffer pool: %u MB at 0x%08x\n", want, c.phys);
            continue;
        }
        if (pool) pool->release();
        piece /= 2;
    }
    IOLog("NativeAdreno430: buffer pool of %u MB in %u pieces\n", got, gPoolCount);
}

// First fit in the first piece it fits in; returns chunk << kChunkShift | first page, or UINT32_MAX.
uint32_t poolTake(uint32_t pages)
{
    if (!gPoolCount) return UINT32_MAX;
    uint32_t found = UINT32_MAX;
    IOLockLock(gPoolLock);
    for (unsigned k = 0; k < gPoolCount && found == UINT32_MAX; ++k) {
        PoolChunk &c = gPool[k];
        uint32_t run = 0;
        for (uint32_t i = 0; i < c.pages; ++i) {
            run = c.map[i] ? 0 : run + 1;
            if (run == pages) {
                const uint32_t first = i + 1 - pages;
                for (uint32_t j = 0; j < pages; ++j) c.map[first + j] = 1;
                gPoolUsed += pages;
                found = k << kChunkShift | first;
                break;
            }
        }
    }
    IOLockUnlock(gPoolLock);
    return found;
}

void poolGive(uint32_t place, uint32_t pages)
{
    PoolChunk &c = gPool[place >> kChunkShift];
    const uint32_t first = place & ((1u << kChunkShift) - 1);
    IOLockLock(gPoolLock);
    for (uint32_t i = 0; i < pages; ++i) c.map[first + i] = 0;
    gPoolUsed -= pages;
    IOLockUnlock(gPoolLock);
}

// A part of the pool as memory of its own: retained, zeroed; its kernel and GPU addresses.
IOMemoryDescriptor *poolMemory(uint32_t place, uint64_t size, uint8_t **kva, uint32_t *phys)
{
    PoolChunk &c = gPool[place >> kChunkShift];
    const uint64_t at = static_cast<uint64_t>(place & ((1u << kChunkShift) - 1)) << 12;
    IOMemoryDescriptor *memory = IOSubMemoryDescriptor::withSubRange(c.memory, at, size, kIODirectionInOut);
    if (memory) {
        *kva = c.kva + at;
        *phys = c.phys + static_cast<uint32_t>(at);
        bzero(*kva, size);
    }
    return memory;
}

uint64_t nowNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_continuous_time(), &ns);
    return ns;
}

uint32_t completed() { return gStore ? gStore[0] : 0; }
bool fenceDone(uint32_t fence) { return static_cast<int32_t>(completed() - fence) >= 0; }

void logState(const char *why)
{
    IOLog("NativeAdreno430: %s: fence %u of %u, rptr 0x%x wptr 0x%x, status 0x%08x int 0x%08x ahb error 0x%08x "
          "scratch2 %u\n", why, completed(), gSubmitted, gr(CP_RB_RPTR), gr(CP_RB_WPTR), gr(RBBM_STATUS),
          gr(RBBM_INT_0_STATUS), gr(RBBM_AHB_ERROR_STATUS), gr(CP_SCRATCH_REG2));
}

// Wait (without gLock) until `fence` is done or the absolute CLOCK_MONOTONIC deadline passes. A GPU whose read
// pointer and fence have not moved for kHangNs while work is outstanding is declared hung: later submissions fail
// instead of freezing their callers.
bool waitFenceLoop(uint32_t fence, uint64_t deadlineNs);
bool waitFence(uint32_t fence, uint64_t deadlineNs)
{
    if (fenceDone(fence)) return true;
    const uint64_t start = nowNs();
    const bool done = waitFenceLoop(fence, deadlineNs);
    const uint64_t took = nowNs() - start;
    ++gWaits;
    gWaitNs += took;
    if (took > gWaitMaxNs) gWaitMaxNs = took;
    return done;
}

// Fences are polled (no interrupt yet). IOSleep(1) lets the timer coalesce: under boot load a 1 ms sleep took
// 16-25 ms (gputest, boots 23/24) and a GL frame 226 ms. So: spin 1 ms, then sleep 50 us at a time with no leeway.
bool waitFenceLoop(uint32_t fence, uint64_t deadlineNs)
{
    uint64_t start = nowNs(), lastMove = start;
    uint32_t lastRptr = gr(CP_RB_RPTR), lastDone = completed();
    for (uint32_t spins = 0;; ++spins) {
        if (fenceDone(fence)) return true;
        if (gHung) return false;
        const uint64_t now = nowNs();
        if (now >= deadlineNs || now - start >= kMaxWaitNs) return false;
        const uint32_t rptr = gr(CP_RB_RPTR), done = completed();
        if (rptr != lastRptr || done != lastDone) {
            lastRptr = rptr;
            lastDone = done;
            lastMove = now;
        } else if (now - lastMove >= kHangNs) {
            IOLockLock(gLock);
            if (!gHung) {
                gHung = true;
                logState("GPU HUNG (no progress for 3 s); later submissions are refused");
            }
            IOLockUnlock(gLock);
            return false;
        }
        if (spins < 200) {
            IODelay(5);
        } else {
            delay_for_interval_with_leeway(50, 0, 1000 /* kMicrosecondScale */);
        }
    }
}

uint64_t deadlineOf(const msm::Timespec &t)
{
    if (t.sec < 0 || t.nsec < 0) return 0;
    return static_cast<uint64_t>(t.sec) * 1000000000ull + static_cast<uint64_t>(t.nsec);
}

// Ring output (gLock held). Space is (rptr - wptr - 1) mod size; packets may straddle the end.
bool ringReserve(uint32_t dwords)
{
    const uint64_t start = nowNs();
    for (;;) {
        const uint32_t rptr = gr(CP_RB_RPTR) % kRingDwords;
        const uint32_t space = (rptr + kRingDwords - gWptr - 1) % kRingDwords;
        if (space >= dwords) return true;
        if (nowNs() - start > kHangNs) {
            gHung = true;
            logState("GPU HUNG (ring full for 3 s)");
            return false;
        }
        IODelay(50);
    }
}
void emit(uint32_t value)
{
    gRing[gWptr] = value;
    gWptr = (gWptr + 1) % kRingDwords;
}
void kick()
{
    __asm__ volatile("dsb sy" ::: "memory");
    gw(CP_RB_WPTR, gWptr);
}

// --- bring-up ---

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
    gw(RBBM_GPU_BUSY_MASKED, 0xffffffff);
    gw(RBBM_SP_HYST_CNT, 0x10);
    gw(RBBM_WAIT_IDLE_CLOCKS_CTL, 0x10);
    gw(RBBM_WAIT_IDLE_CLOCKS_CTL2, 0x30);
    gw(RBBM_AHB_CTL0, 0x00000001);
    gw(RBBM_AHB_CTL1, 0xa6ffffff);
    gw(RBBM_RBBM_CTL, 0x00000030);
    gw(RBBM_INTERFACE_HANG_INT_CTL, (1u << 30) | 0xffff);
    gw(RBBM_PERFCTR_CTL, 0x01);
    gw(CP_PERFCTR_CP_SEL_0, 0);   // CP_ALWAYS_COUNT: MSM_PARAM_TIMESTAMP
    gw(UCHE_CACHE_WAYS_VFD, 0x07);
    gw(UCHE_TRAP_BASE_LO, 0xffff0000);
    gw(UCHE_TRAP_BASE_HI, 0xffff0000);
    gw(CP_DEBUG, (1u << 25) | (1u << 14));
    gw(RBBM_CLOCK_CTL, 0);    // hardware clock gating off for bring-up
    gw(RBBM_CLOCK_CTL2, 0);
    gw(RBBM_INT_0_MASK, 0);   // polled
}

bool bringUp()
{
    if (!mmss.open(kMmssPhys, kMmssSize)) {
        IOLog("NativeAdreno430: mmss window did not map\n");
        return false;
    }
    checkpoint("reading the oxili power domains");
    if (!poweredByLoader()) {
        IOLog("NativeAdreno430: the loader did not power the GPU (needs loader v16); nothing touched\n");
        return false;
    }
    if (!gpu.open(kGpuPhys, kGpuSize)) {
        IOLog("NativeAdreno430: kgsl window did not map\n");
        return false;
    }
    checkpoint("first read of the kgsl window");
    IOLog("NativeAdreno430: hw version 0x%08x status 0x%08x power status 0x%08x me cntl 0x%08x\n",
          gr(RBBM_HW_VERSION), gr(RBBM_STATUS), gr(RBBM_POWER_STATUS), gr(CP_ME_CNTL));
    gw(RBBM_POWER_CNTL_IP, 0x778000);
    bool on = false;
    for (int i = 0; i < 20000 && !on; ++i) {
        on = (gr(RBBM_POWER_STATUS) & kSpTpPwrOn) != 0;
        if (!on) IODelay(5);
    }
    if (!on) {
        IOLog("NativeAdreno430: sp/tp NOT on (power status 0x%08x)\n", gr(RBBM_POWER_STATUS));
        return false;
    }

    // Ring + store page: contiguous, below 4 GiB, write-combined (no CPU cache between us and the GPU).
    gRingMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache, kRingBytes + 0x1000,
        0xfffff000ULL);
    if (!gRingMemory) {
        IOLog("NativeAdreno430: no memory for the ring\n");
        return false;
    }
    IOByteCount length = 0;
    const uint64_t phys = gRingMemory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    gRing = static_cast<volatile uint32_t *>(gRingMemory->getBytesNoCopy());
    if (!gRing || !phys || length < kRingBytes + 0x1000 || (phys >> 32)) {
        IOLog("NativeAdreno430: ring memory unusable (phys 0x%llx, %llu contiguous bytes)\n", phys,
              static_cast<unsigned long long>(length));
        return false;
    }
    gStore = gRing + kRingDwords;
    gRingPhys = static_cast<uint32_t>(phys);
    gStorePhys = gRingPhys + kRingBytes;
    for (uint32_t i = 0; i < kRingDwords + 1024; ++i) gRing[i] = 0;

    hwInit();
    gw(CP_RB_CNTL, kRbCntl);
    gw(CP_RB_BASE, gRingPhys);
    gw(CP_ME_RAM_WADDR, 0);
    for (uint32_t i = 1; i < sizeof kA420Pm4 / 4; ++i) gw(CP_ME_RAM_DATA, kA420Pm4[i]);
    gw(CP_PFP_UCODE_ADDR, 0);
    for (uint32_t i = 1; i < sizeof kA420Pfp / 4; ++i) gw(CP_PFP_UCODE_DATA, kA420Pfp[i]);
    gw(CP_ME_CNTL, 0);

    static const uint32_t meInit[17] = {0x000003f7, 0, 0, 0, 0x00000080, 0x00000100, 0x00000180, 0x00006600, 0x00000150,
                                        0x0000014e, 0x00000154, 0x00000001, 0, 0, 0, 0, 0};
    gWptr = 0;
    emit(pkt3(CP_ME_INIT, 17));
    for (uint32_t value : meInit) emit(value);
    emit(pkt3(CP_MEM_WRITE, 2));
    emit(gStorePhys + 8);
    emit(0xcafe430a);
    emit(pkt3(CP_MEM_WRITE, 2));
    emit(gStorePhys + 12);
    emit(0x12345678);
    checkpoint("CP_ME_INIT and the CP_MEM_WRITE self-test");
    kick();
    bool drained = false;
    for (int i = 0; i < 2000 && !drained; ++i) {
        drained = gr(CP_RB_RPTR) == gWptr && gStore[3] == 0x12345678;
        if (!drained) IOSleep(1);
    }
    const bool pass = drained && gStore[2] == 0xcafe430a;
    IOLog("NativeAdreno430: microcode pm4 0x%08x pfp 0x%08x, ring at 0x%08x: rptr 0x%x wptr 0x%x self-test "
          "0x%08x 0x%08x status 0x%08x: %s\n", kA420Pm4[1], kA420Pfp[5], gRingPhys, gr(CP_RB_RPTR), gWptr, gStore[2],
          gStore[3], gr(RBBM_STATUS), pass ? "PASS, the GPU executes commands" : "FAIL");
    return pass;
}

} // namespace

// ============================================================================================================
// The service
// ============================================================================================================

class NativeAdreno430 final : public IOService {
    OSDeclareDefaultStructors(NativeAdreno430)
    static void bringUpThread(void *self, wait_result_t);
public:
    bool start(IOService *provider) override;
};
OSDefineMetaClassAndStructors(NativeAdreno430, IOService)

void NativeAdreno430::bringUpThread(void *self, wait_result_t)
{
    NativeAdreno430 *service = static_cast<NativeAdreno430 *>(self);
    IOSleep(10000);
    if (bringUp()) {
        gReady = true;
        service->setProperty("GPU", "Adreno 430");
        service->registerService();
        IOLog("NativeAdreno430: ready for clients\n");
    }
    service->release();
}

bool NativeAdreno430::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    uint32_t enabled = 1;
    PE_parse_boot_argn("nativegpu", &enabled, sizeof enabled);
    if (!enabled) {
        IOLog("NativeAdreno430: skipped (nativegpu=0)\n");
        return true;
    }
    gLock = IOLockAlloc();
    if (!gLock) return false;
    poolInit();
    setProperty(kIOUserClientClassKey, "NativeAdreno430UserClient");
    retain();
    thread_t thread = nullptr;
    if (kernel_thread_start(bringUpThread, this, &thread) != KERN_SUCCESS) {
        release();
        IOLog("NativeAdreno430: no bring-up thread\n");
        return true;
    }
    thread_deallocate(thread);
    IOLog("NativeAdreno430: bring-up in 10 s\n");
    return true;
}

// ============================================================================================================
// The user client: msm DRM calls
// ============================================================================================================

namespace {
struct Buffer {
    IOMemoryDescriptor *memory;    // its own, or a part of the pool
    uint8_t *kva;                  // the kernel's address of it
    uint64_t size;
    uint32_t phys, flags, lastFence;
    uint32_t poolPage;             // UINT32_MAX: not from the pool
    OSObject *owner;               // an IOSurface whose memory this is (HAKIOS_GPU_SURFACE), held while the buffer is
};
}

class NativeAdreno430UserClient final : public IOUserClient {
    OSDeclareDefaultStructors(NativeAdreno430UserClient)
    task_t fTask = nullptr;
    Buffer **fBuffers = nullptr;   // index = handle - 1
    // fBuffers and what hangs off it, for the calls of one client: Mesa allocates from one thread while its submit
    // thread looks buffers up. With all 8 cores running (boot 40) two GEM_NEWs could take the same free slot, and Mesa's
    // first submission hung the GPU. Never held while waiting for the GPU.
    IOLock *fTableLock = nullptr;
    uint32_t fCount = 0, fNextQueue = 0;
    // what this client asked to wait for: calls, and how many found the GPU still busy (the rest were done already)
    uint64_t fSubmits = 0, fCuts = 0, fFenceWaits = 0, fFenceBusy = 0, fPreps = 0, fPrepBusy = 0, fPrepNoSync = 0;
    char fName[32] = {};
    int fPid = 0;
    uint64_t fBytes = 0;

    Buffer *buffer(uint32_t handle)
    {
        return handle >= 1 && handle <= kMaxHandles ? fBuffers[handle - 1] : nullptr;
    }
    void freeBuffer(uint32_t handle);
    Buffer *detach(uint32_t handle);
    void destroy(Buffer *b);
    IOReturn gemNew(msm::GemNew &r);
    IOReturn gemInfo(msm::GemInfo &r);
    IOReturn submit(msm::Submit &r);
    IOReturn param(msm::Param &r);
    IOReturn surface(msm::Surface &r);

public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
};
OSDefineMetaClassAndStructors(NativeAdreno430UserClient, IOUserClient)

bool NativeAdreno430UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type)) return false;
    fPid = proc_selfpid();
    proc_selfname(fName, sizeof fName);
    fTask = owningTask;
    return true;
}

bool NativeAdreno430UserClient::start(IOService *provider)
{
    if (!gReady || !IOUserClient::start(provider)) return false;
    fBuffers = IONewZero(Buffer *, kMaxHandles);
    fTableLock = IOLockAlloc();
    return fBuffers != nullptr && fTableLock != nullptr;
}

// Out of the table (fTableLock held, or no other caller left)
Buffer *NativeAdreno430UserClient::detach(uint32_t handle)
{
    Buffer *b = buffer(handle);
    if (!b) return nullptr;
    fBuffers[handle - 1] = nullptr;
    --fCount;
    fBytes -= b->size;
    return b;
}

void NativeAdreno430UserClient::freeBuffer(uint32_t handle)
{
    destroy(detach(handle));
}

void NativeAdreno430UserClient::destroy(Buffer *b)
{
    if (!b) return;
    // The GPU may still use it: wait for its last submission (outside the lock). A hung GPU keeps its memory.
    if (!fenceDone(b->lastFence) && !waitFence(b->lastFence, nowNs() + 2000000000ull)) {
        IOLog("NativeAdreno430: buffer at 0x%08x (%llu bytes) still in use by the GPU; not freed\n", b->phys,
              static_cast<unsigned long long>(b->size));
        IODelete(b, Buffer, 1);
        return;
    }
    if (!b->owner) {                 // (an IOSurface's memory is not counted in: boot 56's count went below 0)
        IOLockLock(gLock);
        gTotalBytes -= b->size;
        IOLockUnlock(gLock);
    }
    b->memory->release();
    if (b->poolPage != UINT32_MAX) poolGive(b->poolPage, static_cast<uint32_t>(b->size >> 12));
    if (b->owner) b->owner->release();
    IODelete(b, Buffer, 1);
}

IOReturn NativeAdreno430UserClient::clientClose()
{
    IOLog("NativeAdreno430: %s[%d] closes: %llu submissions (%llu waits for idle put before Mesa's fence writes); WAIT_FENCE %llu (%llu busy), CPU_PREP %llu (%llu busy, %llu "
          "nosync); all clients: %llu submissions, %llu waits, %llu ms waited, longest %llu us\n", fName, fPid, fSubmits, fCuts,
          fFenceWaits, fFenceBusy, fPreps, fPrepBusy, fPrepNoSync, gSubmits, gWaits, gWaitNs / 1000000, gWaitMaxNs / 1000);
    if (fBuffers) {
        for (uint32_t h = 1; h <= kMaxHandles; ++h) freeBuffer(h);
    }
    terminate();
    return kIOReturnSuccess;
}

void NativeAdreno430UserClient::free()
{
    if (fBuffers) {
        for (uint32_t h = 1; h <= kMaxHandles; ++h) freeBuffer(h);
        IODelete(fBuffers, Buffer *, kMaxHandles);
    }
    if (fTableLock) IOLockFree(fTableLock);
    IOUserClient::free();
}

IOReturn NativeAdreno430UserClient::gemNew(msm::GemNew &r)
{
    if (r.size == 0 || r.size > kMaxBufferBytes) return kIOReturnBadArgument;
    const uint64_t size = (r.size + 0xfff) & ~0xfffull;
    uint32_t slot = 0;
    while (slot < kMaxHandles && fBuffers[slot]) ++slot;
    if (slot == kMaxHandles) return kIOReturnNoResources;
    IOLockLock(gLock);
    const bool room = gTotalBytes + size <= kMaxTotalBytes;
    if (room) gTotalBytes += size;
    IOLockUnlock(gLock);
    if (!room) {
        IOLog("NativeAdreno430: refused a %llu-byte buffer: %llu MB of GPU memory in use\n",
              static_cast<unsigned long long>(size), static_cast<unsigned long long>(gTotalBytes >> 20));
        return kIOReturnNoMemory;
    }
    // From the pool if it fits there (zeroed here), else contiguous on its own (the GPU's addresses are physical while
    // the SMMU does not translate), below 4 GiB, write-combined like Linux's msm buffers; comes zeroed.
    const uint32_t pages = static_cast<uint32_t>(size >> 12);
    const uint32_t first = poolTake(pages);
    IOMemoryDescriptor *memory = nullptr;
    uint8_t *kva = nullptr;
    uint64_t phys = 0;
    IOByteCount length = 0;
    if (first != UINT32_MAX) {
        uint32_t at = 0;
        memory = poolMemory(first, size, &kva, &at);
        if (memory) {
            phys = at;
            length = size;
        } else {
            poolGive(first, pages);
        }
    }
    if (!memory) {
        IOBufferMemoryDescriptor *own = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
            size, 0xfffff000ULL);
        memory = own;
        kva = own ? static_cast<uint8_t *>(own->getBytesNoCopy()) : nullptr;
        phys = own ? own->getPhysicalSegment(0, &length, kIOMemoryMapperNone) : 0;
    }
    if (!memory || !phys || length < size || (phys >> 32)) {
        IOLog("NativeAdreno430: no contiguous memory for a %llu-byte buffer (pool: %u of %u pages used)\n",
              static_cast<unsigned long long>(size), gPoolUsed, gPoolPages);
        if (memory) memory->release();
        IOLockLock(gLock);
        gTotalBytes -= size;
        IOLockUnlock(gLock);
        return kIOReturnNoMemory;
    }
    Buffer *b = IONew(Buffer, 1);
    if (!b) {
        memory->release();
        if (first != UINT32_MAX) poolGive(first, pages);
        IOLockLock(gLock);
        gTotalBytes -= size;
        IOLockUnlock(gLock);
        return kIOReturnNoMemory;
    }
    b->memory = memory;
    b->kva = kva;
    b->poolPage = first;
    b->owner = nullptr;
    b->size = size;
    b->phys = static_cast<uint32_t>(phys);
    b->flags = r.flags;
    b->lastFence = completed();
    fBuffers[slot] = b;
    ++fCount;
    fBytes += size;
    r.handle = slot + 1;
    return kIOReturnSuccess;
}

IOReturn NativeAdreno430UserClient::gemInfo(msm::GemInfo &r)
{
    Buffer *b = buffer(r.handle);
    if (!b) return kIOReturnBadArgument;
    switch (r.info) {
    case msm::INFO_GET_OFFSET:
        r.value = r.handle;   // the shim maps by handle (clientMemoryForType)
        return kIOReturnSuccess;
    case msm::INFO_GET_IOVA:
        r.value = b->phys;
        return kIOReturnSuccess;
    case msm::INFO_GET_FLAGS:
        r.value = b->flags;
        return kIOReturnSuccess;
    case msm::INFO_SET_NAME:
        return kIOReturnSuccess;
    case msm::INFO_GET_NAME:
        r.len = 0;
        return kIOReturnSuccess;
    default:
        return kIOReturnUnsupported;
    }
}

IOReturn NativeAdreno430UserClient::param(msm::Param &r)
{
    switch (r.param) {
    case msm::PARAM_GPU_ID: r.value = kGpuId; return kIOReturnSuccess;
    case msm::PARAM_GMEM_SIZE: r.value = kGmemSize; return kIOReturnSuccess;
    case msm::PARAM_CHIP_ID: r.value = kChipId; return kIOReturnSuccess;
    case msm::PARAM_MAX_FREQ: r.value = kMaxFreq; return kIOReturnSuccess;
    case msm::PARAM_GMEM_BASE: r.value = kGmemBase; return kIOReturnSuccess;
    case msm::PARAM_PRIORITIES: r.value = 1; return kIOReturnSuccess;
    case msm::PARAM_PP_PGTABLE: case msm::PARAM_FAULTS: case msm::PARAM_SUSPENDS: r.value = 0; return kIOReturnSuccess;
    case msm::PARAM_TIMESTAMP: {
        uint32_t hi, lo;
        do {
            hi = gr(RBBM_PERFCTR_CP_0_HI);
            lo = gr(RBBM_PERFCTR_CP_0_LO);
        } while (hi != gr(RBBM_PERFCTR_CP_0_HI));
        r.value = (static_cast<uint64_t>(hi) << 32) | lo;
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnBadArgument;   // EINVAL: what Linux answers for a parameter it does not have
    }
}

// Mesa writes its own fence (pipe->control->fence) with CP_EVENT_WRITE CACHE_FLUSH_TS at the end of each batch and
// trusts it: gltri made 101 submissions and not one WAIT_FENCE or CPU_PREP (boot 32). On the A4xx that event is not
// preceded by a wait for idle, so the fence can land while a draw is still in the pipeline: boot 25 read a frame with
// the clear but without the triangle. Each top-level command buffer is therefore cut before every CACHE_FLUSH_TS
// event write, and HLSQ_FLUSH + CP_WAIT_FOR_IDLE go in between (as before the kernel's own fence). Packets are walked
// by their headers; anything unknown ends the walk, leaving the rest uncut.
constexpr uint32_t kMaxCuts = 4;

// Off by default: the cut hung the GPU on Mesa's first submission (boot 33). Boot-arg adrenocut=1 turns it on.
static bool cutsWanted()
{
    static int wanted = -1;
    if (wanted < 0) {
        uint32_t value = 0;
        PE_parse_boot_argn("adrenocut", &value, sizeof value);
        wanted = value ? 1 : 0;
    }
    return wanted == 1;
}

static uint32_t findTimestampWrites(const uint32_t *ib, uint32_t dwords, uint32_t *at)
{
    uint32_t found = 0, i = 0;
    while (i < dwords && found < kMaxCuts) {
        const uint32_t header = ib[i], type = header >> 30;
        uint32_t length;
        if (type == 3) {
            length = ((header >> 16) & 0x3fff) + 2;
            if (((header >> 8) & 0xff) == CP_EVENT_WRITE && i > 0 && i + 1 < dwords &&
                (ib[i + 1] & 0x3f) == CACHE_FLUSH_TS) {
                at[found++] = i;
            }
        } else if (type == 0) {
            length = ((header >> 16) & 0x3fff) + 2;
        } else if (type == 2) {
            length = 1;
        } else {
            break;
        }
        i += length;
    }
    return found;
}

IOReturn NativeAdreno430UserClient::submit(msm::Submit &r)
{
    if (r.nrBos > kMaxSubmitBos || r.nrCmds == 0 || r.nrCmds > kMaxSubmitCmds) return kIOReturnBadArgument;
    if (r.flags & msm::SUBMIT_FENCE_SN_IN) return kIOReturnUnsupported;
    msm::SubmitBo *bos = r.nrBos ? IONew(msm::SubmitBo, r.nrBos) : nullptr;
    msm::SubmitCmd *cmds = IONew(msm::SubmitCmd, r.nrCmds);
    Buffer **used = r.nrBos ? IONew(Buffer *, r.nrBos) : nullptr;
    uint32_t *cuts = IONew(uint32_t, r.nrCmds * (kMaxCuts + 1));   // per command: count, then dword offsets
    IOReturn ret = kIOReturnSuccess;
    uint32_t totalCuts = 0;
    if ((r.nrBos && (!bos || !used)) || !cmds || !cuts) {
        ret = kIOReturnNoMemory;
    } else if ((r.nrBos && copyin(static_cast<user_addr_t>(r.bos), bos, r.nrBos * sizeof *bos)) ||
               copyin(static_cast<user_addr_t>(r.cmds), cmds, r.nrCmds * sizeof *cmds)) {
        ret = kIOReturnBadArgument;
    }
    for (uint32_t i = 0; ret == kIOReturnSuccess && i < r.nrBos; ++i) {
        used[i] = buffer(bos[i].handle);
        if (!used[i]) ret = kIOReturnBadArgument;
    }
    for (uint32_t i = 0; ret == kIOReturnSuccess && i < r.nrCmds; ++i) {
        const msm::SubmitCmd &c = cmds[i];
        if (c.type < msm::SUBMIT_CMD_BUF || c.type > msm::SUBMIT_CMD_CTX_RESTORE_BUF || c.submitIdx >= r.nrBos ||
            (c.size & 3) || (c.submitOffset & 3) || c.size == 0 ||
            static_cast<uint64_t>(c.submitOffset) + c.size > used[c.submitIdx]->size) {
            ret = kIOReturnBadArgument;
        } else if (c.nrRelocs) {
            IOLog("NativeAdreno430: a submission with relocations (this driver only does softpin)\n");
            ret = kIOReturnUnsupported;
        } else {
            uint32_t *mine = cuts + i * (kMaxCuts + 1);
            mine[0] = 0;
            if (c.type == msm::SUBMIT_CMD_BUF && cutsWanted()) {
                const uint8_t *base = used[c.submitIdx]->kva;
                if (base) {
                    mine[0] = findTimestampWrites(reinterpret_cast<const uint32_t *>(base + c.submitOffset),
                                                  c.size / 4, mine + 1);
                }
            }
            totalCuts += mine[0];
        }
    }
    if (ret == kIOReturnSuccess) {
        IOLockLock(gLock);
        if (gHung) {
            ret = kIOReturnNotReady;
        } else if (!ringReserve(r.nrCmds * 4 + totalCuts * 8 + 12)) {
            ret = kIOReturnTimeout;
        } else {
            for (uint32_t i = 0; i < r.nrCmds; ++i) {
                const msm::SubmitCmd &c = cmds[i];
                if (c.type == msm::SUBMIT_CMD_IB_TARGET_BUF) continue;
                const uint32_t *mine = cuts + i * (kMaxCuts + 1);
                const uint32_t start = used[c.submitIdx]->phys + c.submitOffset, dwords = c.size / 4;
                uint32_t from = 0;
                for (uint32_t k = 0; k <= mine[0]; ++k) {
                    const uint32_t to = k < mine[0] ? mine[1 + k] : dwords;
                    if (to > from) {
                        emit(pkt3(CP_INDIRECT_BUFFER_PFE, 2));
                        emit(start + from * 4);
                        emit(to - from);
                        emit(kPkt2);
                    }
                    if (k < mine[0]) {
                        emit(pkt3(CP_EVENT_WRITE, 1));
                        emit(HLSQ_FLUSH);
                        emit(pkt3(CP_WAIT_FOR_IDLE, 1));
                        emit(0);
                    }
                    from = to;
                }
            }
            const uint32_t fence = ++gSubmitted;
            emit(pkt0(CP_SCRATCH_REG2, 1));
            emit(fence);
            emit(pkt3(CP_EVENT_WRITE, 1));    // flush HLSQ lazy updates before the timestamp
            emit(HLSQ_FLUSH);
            emit(pkt3(CP_WAIT_FOR_IDLE, 1));
            emit(0);
            emit(pkt3(CP_EVENT_WRITE, 3));
            emit(CACHE_FLUSH_TS);
            emit(gStorePhys);
            emit(fence);
            for (uint32_t i = 0; i < r.nrBos; ++i) used[i]->lastFence = fence;
            kick();
            ++gSubmits;
            fCuts += totalCuts;
            r.fence = fence;
            r.fenceFd = -1;
        }
        IOLockUnlock(gLock);
    }
    if (bos) IODelete(bos, msm::SubmitBo, r.nrBos);
    if (used) IODelete(used, Buffer *, r.nrBos);
    if (cmds) IODelete(cmds, msm::SubmitCmd, r.nrCmds);
    if (cuts) IODelete(cuts, uint32_t, r.nrCmds * (kMaxCuts + 1));
    return ret;
}

// HAKIOS_GPU_SURFACE: an IOSurface as a buffer of this client's (Mesa's drmPrimeFDToHandle, the "file descriptor" being the
// surface's ID), or with handle 0xffffffff only what it would be like. Only surfaces whose memory came from the pool are
// the GPU's to address (NativeSurface: the window server's big ones, with suzurangl=1). A 'b3a8' one (blue-first pixels,
// a second plane of compression bookkeeping nothing here uses) is described as its plane 0 of 'BGRA'.
IOReturn NativeAdreno430UserClient::surface(msm::Surface &r)
{
    uint32_t phys = 0, rowBytes = 0, offset = 0, width = 0, height = 0, format = 0, planes = 0;
    uint64_t size = 0;
    uint8_t *kva = nullptr;
    IOMemoryDescriptor *memory = nullptr;
    OSObject *owner = native_surface_gpu(r.surface, &memory, &kva, &phys, &size, &offset, &rowBytes, &width, &height,
                                         &format, &planes);
    static unsigned said;
    if (!owner) {
        if (said++ < 8) IOLog("NativeAdreno430: surface %u is not the GPU's\n", r.surface);
        return kIOReturnUnsupported;
    }
    r.size = size;
    r.offset = offset;
    r.rowBytes = rowBytes;
    r.width = width;
    r.height = height;
    r.pixelFormat = format;
    r.planes = planes;
    if (r.handle == 0xffffffffu) {
        memory->release();
        owner->release();
        return kIOReturnSuccess;
    }
    uint32_t slot = 0;
    while (slot < kMaxHandles && fBuffers[slot]) ++slot;
    Buffer *b = slot < kMaxHandles ? IONew(Buffer, 1) : nullptr;
    if (!b) {
        memory->release();
        owner->release();
        return kIOReturnNoResources;
    }
    b->memory = memory;
    b->kva = kva;
    b->size = size;
    b->phys = phys;
    b->flags = 0;
    b->lastFence = completed();
    b->poolPage = UINT32_MAX;
    b->owner = owner;
    fBuffers[slot] = b;
    ++fCount;
    fBytes += size;
    r.handle = slot + 1;
    if (said++ < 8) IOLog("NativeAdreno430: surface %u as buffer %u: %llu bytes at 0x%08x, %u a row\n", r.surface, r.handle,
                          static_cast<unsigned long long>(size), phys, rowBytes);
    return kIOReturnSuccess;
}

IOReturn NativeAdreno430UserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    IOLockLock(fTableLock);
    Buffer *b = buffer(type);
    if (b) b->memory->retain();
    IOLockUnlock(fTableLock);
    if (!b) return kIOReturnBadArgument;
    *memory = b->memory;
    // (a part of the pool, or an IOSurface's memory taken from it, has no cache mode of its own)
    *options = b->poolPage != UINT32_MAX || b->owner ? kIOMapWriteCombineCache : 0;
    return kIOReturnSuccess;
}

IOReturn NativeAdreno430UserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                                   IOExternalMethodDispatch *, OSObject *, void *)
{
    union {
        msm::Param param;
        msm::GemNew gemNew;
        msm::GemInfo gemInfo;
        msm::GemCpuPrep cpuPrep;
        msm::Submit submit;
        msm::WaitFence wait;
        msm::GemMadvise madvise;
        msm::SubmitQueue queue;
        msm::SubmitQueueQuery query;
        msm::GemClose close;
        msm::Log log;
        msm::Surface surface;
    } r;
    const uint32_t size = args->structureInputSize;
    if (!args->structureInput || size > sizeof r || !fBuffers) return kIOReturnBadArgument;
    if (!gReady) return kIOReturnNotReady;
    bzero(&r, sizeof r);
    memcpy(&r, args->structureInput, size);

    IOReturn ret = kIOReturnSuccess;
    // the table lock for everything but WAIT_FENCE; CPU_PREP and GEM_CLOSE drop it before they wait
    bool locked = selector != msm::kWaitFence;
    if (locked) IOLockLock(fTableLock);
    switch (selector) {
    case msm::kGetParam:
        ret = param(r.param);
        break;
    case msm::kSetParam:
    case msm::kGemCpuFini:
    case msm::kSubmitQueueClose:
        break;
    case msm::kGemNew:
        ret = gemNew(r.gemNew);
        break;
    case msm::kGemInfo:
        ret = gemInfo(r.gemInfo);
        break;
    case msm::kGemCpuPrep: {
        Buffer *b = buffer(r.cpuPrep.handle);
        const uint32_t fence = b ? b->lastFence : 0;
        IOLockUnlock(fTableLock);
        locked = false;
        ++fPreps;
        if (b && !fenceDone(fence)) {
            ++fPrepBusy;
            if (r.cpuPrep.op & msm::PREP_NOSYNC) ++fPrepNoSync;
        }
        if (!b) {
            ret = kIOReturnBadArgument;
        } else if (!fenceDone(fence) &&
                   ((r.cpuPrep.op & msm::PREP_NOSYNC) || !waitFence(fence, deadlineOf(r.cpuPrep.timeout)))) {
            ret = kIOReturnBusy;
        }
        break;
    }
    case msm::kGemSubmit:
        ret = submit(r.submit);
        if (ret == kIOReturnSuccess) ++fSubmits;
        break;
    case msm::kWaitFence:
        ++fFenceWaits;
        if (!fenceDone(r.wait.fence)) ++fFenceBusy;
        if (static_cast<int32_t>(r.wait.fence - gSubmitted) > 0) {
            ret = kIOReturnBadArgument;   // never issued: never done
        } else if (!waitFence(r.wait.fence, deadlineOf(r.wait.timeout))) {
            ret = kIOReturnTimeout;
        }
        break;
    case msm::kGemMadvise:
        r.madvise.retained = 1;
        break;
    case msm::kSubmitQueueNew:
        r.queue.id = ++fNextQueue;
        break;
    case msm::kSubmitQueueQuery:
        if (r.query.data && r.query.len) {
            const uint64_t zero = 0;
            copyout(&zero, static_cast<user_addr_t>(r.query.data), r.query.len < 8 ? r.query.len : 8);
        }
        break;
    case msm::kGemClose: {
        Buffer *b = detach(r.close.handle);
        IOLockUnlock(fTableLock);
        locked = false;
        if (!b) {
            ret = kIOReturnBadArgument;
        } else {
            destroy(b);
        }
        break;
    }
    case msm::kSurface:
        ret = surface(r.surface);
        break;
    case msm::kLog:
        r.log.text[sizeof r.log.text - 1] = 0;
        IOLog("gpu client: %s\n", r.log.text);
        break;
    default:
        IOLog("NativeAdreno430: call 0x%x (%u bytes) is not known here\n", selector, size);
        ret = kIOReturnUnsupported;
        break;
    }
    if (locked) IOLockUnlock(fTableLock);
    if (ret == kIOReturnSuccess && args->structureOutput) {
        if (args->structureOutputSize > size) args->structureOutputSize = size;
        memcpy(args->structureOutput, &r, args->structureOutputSize);
    }
    return ret;
}

// For NativeSurface: memory from the GPU's pool (zeroed, retained; *place to give it back with), or null.
extern "C" IOMemoryDescriptor *native_gpu_memory(uint64_t size, uint8_t **kva, uint32_t *phys, uint32_t *place)
{
    size = (size + 0xfff) & ~0xfffull;
    const uint32_t pages = static_cast<uint32_t>(size >> 12);
    const uint32_t first = poolTake(pages);
    if (first == UINT32_MAX) return nullptr;
    IOMemoryDescriptor *memory = poolMemory(first, size, kva, phys);
    if (!memory) {
        poolGive(first, pages);
        return nullptr;
    }
    *place = first;
    return memory;
}

extern "C" void native_gpu_memory_free(uint32_t place, uint64_t size)
{
    poolGive(place, static_cast<uint32_t>(((size + 0xfff) & ~0xfffull) >> 12));
}
