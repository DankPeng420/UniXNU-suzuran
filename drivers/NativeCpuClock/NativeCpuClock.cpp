// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// CPU clock readout for the Lumia 950 XL (MSM8994): the speed the firmware that boots iOS left the
// two clusters at. Nothing in iOS changes CPU clocks here (no DVFS driver), so whatever the Windows
// Phone UEFI chose is what backboardd's software renderer gets.
//
// Two independent readings, both read-only:
//   1. the cluster PLLs and muxes (cityman kernel, clock-cpu-8994.c and the qcom,cpu-clock-8994 node
//      of msm8994.dtsi): each PLL's mode, L value (early output = L x 19.2 MHz) and alpha, and the
//      GLB mux register that picks the CPU source,
//   2. a timed chain of dependent adds (one cycle each on A53 and A57) on every core the measuring
//      threads land on, against the architectural counter, with interrupts off: MHz per MPIDR.
// Boot 13 (2026-10-31): both cluster PLLs at 1536 MHz behind the low-frequency mux, A53 at 768 MHz
// (post-divider 2), A57 at 384 MHz (post-divider 4). Going faster needs more voltage, so this also
// logs what deciding that needs (msm8994-regulator.dtsi, msm8994-v2-pm.dtsi, spm-regulator.c):
//   3. the CPU rails, PM8994 S8 (A53) and S9-S11 (A57, S11 leads), and S2 (MX), read through
//      NativeTouch's SPMI channel (native_spmi_read): type, range, set point, mode, enable,
//   4. the memory-assist (mem-acc) and CPR clock registers beside each cluster mux,
//   5. the QFPROM rows holding the speed bins and the CPR fuses (target quotients, open-loop
//      voltages, revision, redundancy), raw, to compute the per-chip voltages offline,
//   6. the SAW2 blocks that carry the rails' voltage requests: the CCI SAW (A53 rail) and the A57 L2 SAW.
// Boot 14: S8 (A53) 1000 mV, S11 (A57) 900 mV, S2 (MX) 1035 mV, all FTS2.5; SAW2 control 0 (the
// rails take direct SPMI writes); fuse revision 4, speed bin 0, no redundancy. The fused open-loop
// voltages per fuse corner (ref + signed steps x 10 mV + the v2 adjustment, within floor/ceiling):
// A53 780 / 855 / 885 / 1115 mV, A57 810 / 810 / 1000 / 1080 mV.
// Boot 15: A57 at 960 MHz (fuse corner 3) on 1025 mV measured 960 MHz and ran ~6 minutes, then the
// SoC reset itself (PMIC power-off reason PS_HOLD, no panic). Above fuse corner 2 Linux also moves
// the A57 memory-assist settings (mem-acc-regulator.c: type1-3 0x02 -> 0x06), which it writes
// through the secure monitor (scm_io_write), and wants MX nominal; this driver does neither. So the
// A57 cluster stays inside fuse corner 2, whose top is virtual corner 5, 633.6 MHz, at the rail the
// firmware set for it (900 mV = the corner's ceiling): nothing but the clock changes.
//   8. unless nativecpufreq=0: the A57 rail must read at least 900 mV, then the A57 cluster's second
//      PLL (PLLA, idle; its VCO runs 1209.6-1996.8 MHz) goes to L 66 = 1267.2 MHz with the main
//      output divided by 2 = 633.6 MHz and the v2 configuration of clock-cpu-8994.c (init_v2_data),
//      and the low-frequency mux moves from PLL0's main output (select 2) onto PLL1's (select 1).
//      Each step is checked and logged after a console flush; a PLL that does not lock is turned
//      off again and the cluster stays where it was.
// Charging under iOS (boot 14) is NativeBattery's.
// One thread per core is started at once so the scheduler spreads them. Runs once, 60 s after
// start, after a pause for the console log to reach the eMMC. Boot-arg nativecpuclock=0 skips it.
#ifndef NATIVE_TARGET_SUZURAN
#define NATIVE_TARGET_SUZURAN 1   // this fork targets the Xperia Z5 Compact; -DNATIVE_TARGET_SUZURAN=0 builds the Lumia values
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>

extern "C" boolean_t ml_set_interrupts_enabled(boolean_t enable);

namespace {

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

// reg / reg-names of qcom,cpu-clock-8994: c0 = A53 cluster, c1 = A57 cluster
constexpr uint64_t kC0Pll = 0xf9015000, kC1Pll = 0xf9016000, kCciPll = 0xf9011000;
constexpr uint64_t kC0Mux = 0xf900d000, kC1Mux = 0xf900f000, kCciMux = 0xf9112000;
constexpr uint32_t kPllMode = 0x0, kPllL = 0x4, kPllAlpha = 0x8, kPllUserCtl = 0x10, kPllStatus = 0x1c;
constexpr uint32_t kPllA = 0x40;                    // the second PLL of each block (PLLA, "pll1")
constexpr uint32_t kMuxOffset = 0x54;               // MUX_OFFSET: lf sel [2:1], hf sel [4:3], div [9:8]
constexpr uint32_t kXoKHz = 19200;

void logPll(const char *name, const Window &w, uint32_t at)
{
    const uint32_t l = w.read(at + kPllL);
    IOLog("NativeCpuClock: %s pll: mode 0x%08x l %u (%u MHz early) alpha 0x%08x user 0x%08x status 0x%08x\n",
          name, w.read(at + kPllMode), l, l * kXoKHz / 1000, w.read(at + kPllAlpha), w.read(at + kPllUserCtl),
          w.read(at + kPllStatus));
}

void logClusterRegisters()
{
    Window c0pll, c1pll, ccipll, c0mux, c1mux, ccimux;
    if (!c0pll.open(kC0Pll, 0x1000) || !c1pll.open(kC1Pll, 0x1000) || !ccipll.open(kCciPll, 0x1000) ||
        !c0mux.open(kC0Mux, 0x1000) || !c1mux.open(kC1Mux, 0x1000) || !ccimux.open(kCciMux, 0x1000)) {
        IOLog("NativeCpuClock: a clock register window did not map\n");
        return;
    }
    IOLog("NativeCpuClock: reading the cluster PLLs\n");
    IOSleep(2500);   // console log flush before the first access, in case it hangs the bus
    logPll("a53 pll0", c0pll, 0);
    logPll("a53 pll1", c0pll, kPllA);
    logPll("a57 pll0", c1pll, 0);
    logPll("a57 pll1", c1pll, kPllA);
    logPll("cci", ccipll, 0);
    const uint32_t a53 = c0mux.read(kMuxOffset), a57 = c1mux.read(kMuxOffset);
    IOLog("NativeCpuClock: mux a53 0x%08x (lf %u hf %u div %u), a57 0x%08x (lf %u hf %u div %u), cci 0x%08x\n",
          a53, (a53 >> 1) & 3, (a53 >> 3) & 3, (a53 >> 8) & 3, a57, (a57 >> 1) & 3, (a57 >> 3) & 3,
          (a57 >> 8) & 3, ccimux.read(kMuxOffset));
}

// --- what raising the clocks depends on (read-only) ---

extern "C" bool native_spmi_read(uint8_t slave, uint16_t address, uint8_t *value) __attribute__((weak));

// SMPS n of the PM8994 at SID 1, 0x1400 + 0x300 * (n - 1); FTS2 (type 0x1c subtype 0x08) set point
// steps are 5 mV (range 0) or 10 mV (range 1) from 0, FTS2.5 (subtype 0x09) 5 or 10 mV from 80 or 160 mV.
void logRail(const char *name, uint32_t n)
{
    const uint16_t base = uint16_t(0x1400 + 0x300 * (n - 1));
    uint8_t type = 0, subtype = 0, status = 0, range = 0, vset = 0, mode = 0, enable = 0;
    if (!native_spmi_read || !native_spmi_read(1, base + 0x04, &type) || !native_spmi_read(1, base + 0x05, &subtype) ||
        !native_spmi_read(1, base + 0x08, &status) || !native_spmi_read(1, base + 0x40, &range) ||
        !native_spmi_read(1, base + 0x41, &vset) || !native_spmi_read(1, base + 0x45, &mode) ||
        !native_spmi_read(1, base + 0x46, &enable)) {
        IOLog("NativeCpuClock: %s (S%u, SID1 0x%04x): SPMI read failed\n", name, n, base);
        return;
    }
    const uint32_t step = (range & 1) ? 10 : 5, offset = subtype == 0x09 ? ((range & 1) ? 160 : 80) : 0;
    IOLog("NativeCpuClock: %s (S%u, SID1 0x%04x): type 0x%02x/0x%02x status 0x%02x range %u vset 0x%02x "
          "(%u mV) mode 0x%02x enable 0x%02x\n", name, n, base, type, subtype, status, range, vset,
          offset + vset * step, mode, enable);
}

constexpr uint64_t kQfprom = 0xfc4bc000, kSpeedBinFuse = 0xfc4b80b0;
constexpr uint64_t kCciSaw = 0xf9065000, kA57Saw = 0xf9013000;

void logSaw(const char *name, const Window &w)
{
    IOLog("NativeCpuClock: %s: ver 0x%08x ctl 0x%08x sts 0x%08x 0x%08x 0x%08x vctl 0x%08x pmic data "
          "0x%08x 0x%08x 0x%08x\n", name, w.read(0xfd0), w.read(0x30), w.read(0x0c), w.read(0x10), w.read(0x14),
          w.read(0x1c), w.read(0x40), w.read(0x44), w.read(0x48));
}

void logDependencies()
{
    logRail("a53 rail", 8);
    logRail("a57 rail", 11);
    logRail("a57 rail phase", 9);
    logRail("a57 rail phase", 10);
    logRail("mx", 2);

    Window c0mux, c1mux, qfprom, speed, cciSaw, a57Saw;
    if (!c0mux.open(kC0Mux, 0x1000) || !c1mux.open(kC1Mux, 0x1000) || !qfprom.open(kQfprom, 0x1000) ||
        !speed.open(kSpeedBinFuse & ~0xfffull, 0x1000) || !cciSaw.open(kCciSaw, 0x1000) ||
        !a57Saw.open(kA57Saw, 0x1000)) {
        IOLog("NativeCpuClock: a fuse or SAW window did not map\n");
        return;
    }
    IOLog("NativeCpuClock: mem-acc a53 %02x %02x %02x %02x cpr clk 0x%08x, a57 %02x %02x %02x %02x cpr clk 0x%08x\n",
          c0mux.read(0x84) & 255, c0mux.read(0x88) & 255, c0mux.read(0x8c) & 255, c0mux.read(0x90) & 255,
          c0mux.read(0x64), c1mux.read(0x84) & 255, c1mux.read(0x88) & 255, c1mux.read(0x8c) & 255,
          c1mux.read(0x90) & 255, c1mux.read(0x64));
    IOLog("NativeCpuClock: reading QFPROM\n");
    IOSleep(2500);   // console log flush before the first access
    const uint32_t at = uint32_t(kSpeedBinFuse & 0xfff);
    IOLog("NativeCpuClock: speed-bin fuse 0x%08x%08x\n", speed.read(at + 4), speed.read(at));
    static const uint32_t rows[] = {22, 56, 57, 62, 136, 137, 138, 139, 140};
    for (uint32_t row : rows) {
        IOLog("NativeCpuClock: qfprom row %u 0x%08x%08x\n", row, qfprom.read(row * 8 + 4), qfprom.read(row * 8));
    }
    logSaw("cci saw (a53 rail)", cciSaw);
    logSaw("a57 l2 saw", a57Saw);
}

// --- the A57 cluster to 633.6 MHz ---

constexpr uint16_t kS11 = 0x3200;                      // the A57 rail's gang leader
constexpr uint32_t kA57MinMV = 900, kA57TargetL = 66;   // 1267.2 MHz VCO, main /2 = 633.6 MHz
constexpr uint32_t kModeOutCtrl = 1u << 0, kModeBypassNl = 1u << 1, kModeResetN = 1u << 2, kModeLock = 1u << 31;
constexpr uint32_t kPllConfigCtl = 0x14, kPllTestLo = 0x20, kPllTestHi = 0x24;
constexpr uint32_t kV2ConfigCtl = 0x004d6968, kV2TestLo = 0x80000000, kV2TestHi = 0x1;
constexpr uint32_t kPostDivShift = 8, kPostDiv2 = 1;   // USER_CTL [9:8]: 1 = /2, 3 = /4

uint32_t railMV(uint8_t vset) { return 80 + 5u * vset; }   // FTS2.5, range 0

bool checkA57Rail()
{
    uint8_t type = 0, subtype = 0, range = 0, vset = 0;
    if (!native_spmi_read || !native_spmi_read(1, kS11 + 0x04, &type) || !native_spmi_read(1, kS11 + 0x05, &subtype) ||
        !native_spmi_read(1, kS11 + 0x40, &range) || !native_spmi_read(1, kS11 + 0x41, &vset)) {
        IOLog("NativeCpuClock: a57 rail unreadable, clocks unchanged\n");
        return false;
    }
    if (type != 0x1c || subtype != 0x09 || range != 0 || railMV(vset) < kA57MinMV) {
        IOLog("NativeCpuClock: a57 rail 0x%02x/0x%02x range %u at %u mV, not the expected FTS2.5 at %u mV or more, "
              "clocks unchanged\n", type, subtype, range, railMV(vset), kA57MinMV);
        return false;
    }
    return true;
}

void raiseA57()
{
    if (!checkA57Rail()) return;
    Window pll, mux;
    if (!pll.open(kC1Pll, 0x1000) || !mux.open(kC1Mux, 0x1000)) {
        IOLog("NativeCpuClock: a57 clock windows did not map\n");
        return;
    }
    if (pll.read(kPllA + kPllMode) & (kModeOutCtrl | kModeResetN)) {
        IOLog("NativeCpuClock: a57 pll1 already running (mode 0x%08x), clocks unchanged\n", pll.read(kPllA + kPllMode));
        return;
    }
    pll.write(kPllA + kPllMode, 0);
    pll.write(kPllA + kPllL, kA57TargetL);
    pll.write(kPllA + kPllAlpha, 0);
    IOLog("NativeCpuClock: a57 pll0 config 0x%08x test 0x%08x 0x%08x; pll1 gets the v2 values\n",
          pll.read(kPllConfigCtl), pll.read(kPllTestLo), pll.read(kPllTestHi));
    pll.write(kPllA + kPllConfigCtl, kV2ConfigCtl);
    pll.write(kPllA + kPllTestLo, kV2TestLo);
    pll.write(kPllA + kPllTestHi, kV2TestHi);
    pll.write(kPllA + kPllUserCtl, (pll.read(kPllUserCtl) & ~(3u << kPostDivShift)) | (kPostDiv2 << kPostDivShift));
    pll.write(kPllA + kPllMode, kModeBypassNl);
    IODelay(10);
    pll.write(kPllA + kPllMode, kModeBypassNl | kModeResetN);
    bool locked = false;
    for (int i = 0; i < 1000 && !locked; ++i) {
        locked = (pll.read(kPllA + kPllMode) & kModeLock) != 0;
        if (!locked) IODelay(1);
    }
    if (!locked) {
        IOLog("NativeCpuClock: a57 pll1 did not lock (mode 0x%08x), turned off, clocks unchanged\n",
              pll.read(kPllA + kPllMode));
        pll.write(kPllA + kPllMode, 0);
        return;
    }
    pll.write(kPllA + kPllMode, kModeBypassNl | kModeResetN | kModeOutCtrl);
    IOLog("NativeCpuClock: a57 pll1 locked at L %u (mode 0x%08x), switching the a57 mux\n", pll.read(kPllA + kPllL),
          pll.read(kPllA + kPllMode));
    IOSleep(2500);
    const uint32_t before = mux.read(kMuxOffset);
    // The Lumia firmware leaves the A57s on PLL0's main output (lf select 2); the Z5C's S1 boots them
    // on the aux source (lf select 0). Either way PLL1 is unused and the switch is the same.
    const uint32_t lf = (before >> 1) & 3;
    if (((before >> 3) & 3) != 0 || (lf != 2 && !(NATIVE_TARGET_SUZURAN && lf == 0))) {
        IOLog("NativeCpuClock: a57 mux 0x%08x is not lf / pll0 main, clocks unchanged\n", before);
        return;
    }
    mux.write(kMuxOffset, (before & ~(3u << 1)) | (1u << 1));
    IODelay(10);
    IOLog("NativeCpuClock: a57 mux 0x%08x -> 0x%08x\n", before, mux.read(kMuxOffset));
}

// --- timed dependent adds ---

constexpr uint32_t kThreads = 8, kRounds = 12, kLoops = 200;   // 200 x 1000 adds per sample

uint64_t counter()
{
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

struct Sample { uint32_t mpidr, minMHz, maxMHz, count; };

uint32_t sampleMHz(uint32_t &mpidr)
{
    const boolean_t was = ml_set_interrupts_enabled(false);
    uint64_t id, frequency, value = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(id));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    const uint64_t start = counter();
    for (uint32_t loop = 0; loop < kLoops; ++loop) {
        __asm__ volatile(".rept 1000\n add %0, %0, #1\n .endr" : "+r"(value));
    }
    const uint64_t ticks = counter() - start;
    ml_set_interrupts_enabled(was);
    mpidr = uint32_t(id & 0xffffff);
    return ticks ? uint32_t(uint64_t(kLoops) * 1000 * frequency / ticks / 1000000) : 0;
}

uint32_t gDone;

void measureThread(void *, wait_result_t)
{
    Sample seen[kThreads] = {};
    for (uint32_t round = 0; round < kRounds; ++round) {
        uint32_t mpidr = 0;
        const uint32_t mhz = sampleMHz(mpidr);
        for (Sample &s : seen) {
            if (s.count && s.mpidr != mpidr) continue;
            if (!s.count || mhz < s.minMHz) s.minMHz = mhz;
            if (mhz > s.maxMHz) s.maxMHz = mhz;
            s.mpidr = mpidr;
            ++s.count;
            break;
        }
        IODelay(2000);   // stay runnable so the other threads keep the other cores
    }
    for (const Sample &s : seen) {
        if (s.count) {
            IOLog("NativeCpuClock: cpu mpidr 0x%03x: %u-%u MHz (%u samples)\n", s.mpidr, s.minMHz, s.maxMHz,
                  s.count);
        }
    }
    __atomic_add_fetch(&gDone, 1, __ATOMIC_RELAXED);
}

void readoutThread(void *, wait_result_t)
{
    IOSleep(60000);   // after NativeTouch's probe, with the home screen up
    logClusterRegisters();
    logDependencies();
    uint32_t raise = 1;
    PE_parse_boot_argn("nativecpufreq", &raise, sizeof raise);
    if (raise) {
        raiseA57();
    } else {
        IOLog("NativeCpuClock: clocks left as found (nativecpufreq=0)\n");
    }
    IOLog("NativeCpuClock: timing %u threads\n", kThreads);
    for (uint32_t i = 0; i < kThreads; ++i) {
        thread_t thread = nullptr;
        if (kernel_thread_start(measureThread, nullptr, &thread) != KERN_SUCCESS) {
            IOLog("NativeCpuClock: no measuring thread %u\n", i);
            __atomic_add_fetch(&gDone, 1, __ATOMIC_RELAXED);
            continue;
        }
        thread_deallocate(thread);
    }
    for (int wait = 0; wait < 100 && __atomic_load_n(&gDone, __ATOMIC_RELAXED) < kThreads; ++wait) IOSleep(50);
    IOLog("NativeCpuClock: done\n");
}

}

class NativeCpuClock final : public IOService {
    OSDeclareDefaultStructors(NativeCpuClock)
public:
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider)) return false;
        uint32_t enabled = 1;
        PE_parse_boot_argn("nativecpuclock", &enabled, sizeof enabled);
        if (!enabled) {
            IOLog("NativeCpuClock: readout disabled (nativecpuclock=0)\n");
            return true;
        }
        thread_t thread = nullptr;
        if (kernel_thread_start(readoutThread, nullptr, &thread) != KERN_SUCCESS) {
            IOLog("NativeCpuClock: no readout thread\n");
            return true;
        }
        thread_deallocate(thread);
        IOLog("NativeCpuClock: readout in 60 s\n");
        return true;
    }
};
OSDefineMetaClassAndStructors(NativeCpuClock, IOService)
