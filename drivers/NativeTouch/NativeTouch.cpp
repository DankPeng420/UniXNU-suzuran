// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Touchscreen and side keys for the Lumia 950 XL: the Synaptics RMI4 controller (S3708AR) polled over
// I2C, its F12 finger objects handed to NativeHID (native_hid_touch), which backboardd reads; and the
// four side keys read from the PM8994 over SPMI in the same poll, handed to native_hid_key.
//
// The controller sits at I2C address 0x4b on BLSP1 QUP1 (0xf9923000), with reset on TLMM GPIO 38
// and its interrupt on GPIO 77; its supplies are PM8994 L22 (3.0 V) and LVS1 (the 1.8 V I2C
// pull-up), both RPM-managed (cityman kernel, arch/arm64/boot/dts/lumia/common/touch.dtsi). The
// firmware that boots iOS (the Windows Phone UEFI) leaves L22 on and the QUP clocked (boot 5, 2026-10-31),
// so this driver writes no supply and only does the minimum to talk to the bus:
//   1. the two supplies, read over SPMI (no writes),
//   2. the BLSP1 AHB and QUP1 I2C clocks, the pin mux of GPIO 2/3 (SDA/SCL) and GPIO 38/77,
//   3. if the clocks are off it turns them on (vote bit 17, branch enable, XO for the QUP root),
//      muxes GPIO 2/3 to the QUP, and takes the controller out of reset if GPIO 38 holds it low,
//   4. polled FIFO-mode I2C (QUP tags v2, 100 kHz) reads of the RMI4 page description table,
//   5. at 400 kHz, F12's register layout, then a ~80 Hz poll of F01's interrupt status and F12's objects.
// Every step is logged, and the step before a first access to a block sleeps long enough for the
// console log to reach the eMMC: a bus hang there freezes the CPU before anything else is written.
// The QUP sequence follows the downstream i2c-msm-v2.c the cityman kernel uses. Boot-arg
// nativetouch=0 skips the probe.
//
// Side keys (cityman kernel, msm-pm8994.dtsi qcom,power-on@800 and lumia/common/sidekeys.dtsi):
// power and volume down are the PON block's KPDPWR and RESIN, volume up and the camera button are
// PM8994 GPIO 3 and 4 (inputs pulled up, low = pressed). They are read-only, polled with the touch
// controller every 12 ms and passed on after two equal reads: power, volume up/down, and the camera
// button as Home (the iPhone X identity has no Home button, but SpringBoard still takes the event).
#ifndef NATIVE_TARGET_SUZURAN
#define NATIVE_TARGET_SUZURAN 1   // this fork targets the Xperia Z5 Compact; -DNATIVE_TARGET_SUZURAN=0 builds the Lumia values
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>

namespace {

constexpr uint64_t kGccPhys = 0xfc400000, kGccSize = 0x2000;
constexpr uint64_t kTlmmPhys = 0xfd510000, kTlmmSize = 0x4000;
// Xperia Z5 Compact (suzuran, NATIVE_TARGET_SUZURAN): the ClearPad sits at 0x2c on BLSP1 QUP6
// (0xf9928000), SDA/SCL GPIO 27/28, interrupt GPIO 42, reset GPIO 109 ACTIVE HIGH (Sony kitakami
// DTS: reset state output-high, running output-low). The loader powers it (L22, S4) and releases reset.
#if NATIVE_TARGET_SUZURAN
constexpr uint64_t kQupPhys = 0xf9928000, kQupSize = 0x1000;
#else
constexpr uint64_t kQupPhys = 0xf9923000, kQupSize = 0x1000;
#endif
constexpr uint64_t kSpmiPhys = 0xfc4c0000, kSpmiSize = 0x10000;

// GCC (clock-gcc-8994.c)
constexpr uint32_t kBranchVote = 0x1484, kBlsp1AhbVoteBit = 1u << 17;
#if NATIVE_TARGET_SUZURAN
// QUP6's branch and root (clock-gcc-8994.c); the k*Qup1* names are kept for the shared code below.
constexpr uint32_t kBlsp1AhbCbcr = 0x05c4, kQup1I2cCbcr = 0x08c8;
constexpr uint32_t kQup1I2cCmdRcgr = 0x08e0, kQup1I2cCfgRcgr = 0x08e4;
#else
constexpr uint32_t kBlsp1AhbCbcr = 0x05c4, kQup1I2cCbcr = 0x0648;
constexpr uint32_t kQup1I2cCmdRcgr = 0x0660, kQup1I2cCfgRcgr = 0x0664;
#endif
constexpr uint32_t kCbcrOff = 1u << 31, kCbcrEnable = 1u << 0;
constexpr uint32_t kRcgUpdate = 1u << 0, kRcgRootOff = 1u << 31;

// TLMM (msm-tlmm-8974 layout): CFG pull[1:0] func[5:2] drive[8:6] oe[9]; IN_OUT in[0] out[1]
constexpr uint32_t tlmmCfg(uint32_t gpio) { return 0x1000 + 0x10 * gpio; }
constexpr uint32_t tlmmInOut(uint32_t gpio) { return 0x1004 + 0x10 * gpio; }
#if NATIVE_TARGET_SUZURAN
constexpr uint32_t kSda = 27, kScl = 28, kReset = 109, kIrq = 42, kQupFunc = 3;
constexpr bool kResetActiveHigh = true;
#else
constexpr uint32_t kSda = 2, kScl = 3, kReset = 38, kIrq = 77, kQupFunc = 3;
constexpr bool kResetActiveHigh = false;
#endif

// QUP (i2c-msm-v2.h)
constexpr uint32_t kConfig = 0x0, kState = 0x4, kIoModes = 0x8, kSwReset = 0xc, kOperational = 0x18,
    kErrorFlags = 0x1c, kErrorFlagsEn = 0x20, kOperationalMask = 0x28, kHwVersion = 0x30,
    kMxOutputCount = 0x100, kOutFifo = 0x110, kMxWriteCount = 0x150, kMxInputCount = 0x200,
    kMxReadCount = 0x208, kInFifo = 0x218, kMasterClkCtl = 0x400, kI2cStatus = 0x404,
    kMasterConfig = 0x408;
constexpr uint32_t kStateReset = 0, kStateRun = 1, kStatePause = 3, kStateMask = 3,
    kStateValid = 1u << 2, kMastGen = 1u << 4;
constexpr uint32_t kConfigI2c = 0x200 | 0x7, kNoInput = 1u << 7, kNoOutput = 1u << 6;
constexpr uint32_t kModeBits = (3u << 10) | (3u << 12) | (1u << 16), kPackUnpack = (1u << 14) | (1u << 15);
constexpr uint32_t kMaxInputDone = 1u << 11, kServiceFlags = (1u << 8) | (1u << 9);
constexpr uint32_t kStatusErrors = 0x380003c, kNacked = 1u << 3;
constexpr uint32_t kClk100k = (62u << 16) | 124u;   // ht_div, fs_div at a 19.2 MHz core clock
constexpr uint32_t kClk400k = (14u << 16) | 28u;
constexpr uint32_t kMaxOutputDone = 1u << 10;
constexpr uint8_t kTagStart = 0x81, kTagWrite = 0x82, kTagWriteStop = 0x83, kTagReadStop = 0x87;
constexpr uint32_t kMaxRead = 60;   // + 2 tag bytes in the 64-byte input FIFO (io modes 0xc0a5)

#if NATIVE_TARGET_SUZURAN
constexpr uint8_t kTouchAddress = 0x2c;
#else
constexpr uint8_t kTouchAddress = 0x4b;
#endif

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

Window gcc, tlmm, qup, spmi;

// Wait for the console log flush (once a second) before an access that could hang the bus.
void checkpoint(const char *what)
{
    IOLog("NativeTouch: %s\n", what);
    IOSleep(2500);
}

bool poll(const Window &w, uint32_t offset, uint32_t mask, uint32_t want, uint32_t tries)
{
    while (tries--) {
        if ((w.read(offset) & mask) == want) return true;
        IODelay(10);
    }
    return false;
}

// --- PM8994 / PMI8994 over SPMI (read-only; the same arbiter command channel as NativeAdreno418) ---

// One command channel, several users (the touch supplies and side keys here, NativeCpuClock's rails
// and NativeBattery's charger through native_spmi_read): one command at a time.
IOLock *gSpmiLock;

bool spmiReadLocked(uint8_t slave, uint16_t address, uint8_t &value)
{
    if (!spmi.base && !spmi.open(kSpmiPhys, kSpmiSize)) return false;
    if (spmi.read(0xf804)) return false;   // channel configured by someone else
    spmi.write(0xf800, (1u << 27) | (uint32_t(slave) << 20) | (uint32_t(address >> 8) << 12) |
                       (uint32_t(address & 255) << 4));
    for (int i = 0; i < 1000; ++i) {
        const uint32_t status = spmi.read(0xf808);
        if (status) {
            if (status != 1) return false;
            value = uint8_t(spmi.read(0xf818) & 255);
            return true;
        }
        IODelay(1);
    }
    return false;
}

bool spmiRead(uint8_t slave, uint16_t address, uint8_t &value)
{
    if (!gSpmiLock) return false;
    IOLockLock(gSpmiLock);
    const bool ok = spmiReadLocked(slave, address, value);
    IOLockUnlock(gSpmiLock);
    return ok;
}

void logSupply(const char *name, uint16_t base)
{
    uint8_t type = 0, subtype = 0, status = 0, range = 0, vset = 0, mode = 0, enable = 0;
    const bool ok = spmiRead(1, base + 0x04, type) && spmiRead(1, base + 0x05, subtype) &&
        spmiRead(1, base + 0x08, status) && spmiRead(1, base + 0x40, range) &&
        spmiRead(1, base + 0x41, vset) && spmiRead(1, base + 0x45, mode) && spmiRead(1, base + 0x46, enable);
    if (!ok) {
        IOLog("NativeTouch: %s (SID1 0x%04x): SPMI read failed\n", name, base);
        return;
    }
    IOLog("NativeTouch: %s (SID1 0x%04x): type 0x%02x/0x%02x status 0x%02x (%s) range %u vset 0x%02x "
          "mode 0x%02x enable 0x%02x (%s)\n", name, base, type, subtype, status,
          (status & 0x80) ? "ready" : "not ready", range, vset, mode, enable, (enable & 0x80) ? "on" : "off");
}

// --- clocks and pins ---

void logPin(const char *name, uint32_t gpio)
{
    const uint32_t cfg = tlmm.read(tlmmCfg(gpio)), io = tlmm.read(tlmmInOut(gpio));
    IOLog("NativeTouch: gpio %u (%s): cfg 0x%03x func %u pull %u drive %umA %s, in %u out %u\n", gpio, name,
          cfg, (cfg >> 2) & 15, cfg & 3, ((cfg >> 6) & 7) * 2 + 2, (cfg & (1u << 9)) ? "output" : "input",
          io & 1, (io >> 1) & 1);
}

bool clocksOn()
{
    IOLog("NativeTouch: gcc vote 0x%08x (blsp1 ahb %s), blsp1 ahb cbcr 0x%08x, qup1 i2c cbcr 0x%08x, "
          "rcgr cmd 0x%08x cfg 0x%08x\n", gcc.read(kBranchVote),
          (gcc.read(kBranchVote) & kBlsp1AhbVoteBit) ? "voted" : "not voted", gcc.read(kBlsp1AhbCbcr),
          gcc.read(kQup1I2cCbcr), gcc.read(kQup1I2cCmdRcgr), gcc.read(kQup1I2cCfgRcgr));
    if (gcc.read(kBlsp1AhbCbcr) & kCbcrOff) {
        gcc.write(kBranchVote, gcc.read(kBranchVote) | kBlsp1AhbVoteBit);
        if (!poll(gcc, kBlsp1AhbCbcr, kCbcrOff, 0, 10000)) {
            IOLog("NativeTouch: blsp1 ahb clock stays off after the vote (cbcr 0x%08x)\n", gcc.read(kBlsp1AhbCbcr));
            return false;
        }
    }
    if (gcc.read(kQup1I2cCmdRcgr) & kRcgRootOff || (gcc.read(kQup1I2cCfgRcgr) & 0x71f) != 0x001) {
        gcc.write(kQup1I2cCfgRcgr, (gcc.read(kQup1I2cCfgRcgr) & ~0x71fu) | 0x001);   // XO, divide by 1
        gcc.write(kQup1I2cCmdRcgr, gcc.read(kQup1I2cCmdRcgr) | kRcgUpdate);
        if (!poll(gcc, kQup1I2cCmdRcgr, kRcgUpdate, 0, 10000)) {
            IOLog("NativeTouch: qup1 i2c root did not take the XO configuration\n");
        }
    }
    if (gcc.read(kQup1I2cCbcr) & kCbcrOff) {
        gcc.write(kQup1I2cCbcr, gcc.read(kQup1I2cCbcr) | kCbcrEnable);
        if (!poll(gcc, kQup1I2cCbcr, kCbcrOff, 0, 10000)) {
            IOLog("NativeTouch: qup1 i2c clock stays off (cbcr 0x%08x, rcgr 0x%08x)\n", gcc.read(kQup1I2cCbcr),
                  gcc.read(kQup1I2cCmdRcgr));
            return false;
        }
    }
    IOLog("NativeTouch: clocks on: blsp1 ahb cbcr 0x%08x, qup1 i2c cbcr 0x%08x, rcgr 0x%08x/0x%08x\n",
          gcc.read(kBlsp1AhbCbcr), gcc.read(kQup1I2cCbcr), gcc.read(kQup1I2cCmdRcgr), gcc.read(kQup1I2cCfgRcgr));
    return true;
}

void pinsForBus()
{
    const uint32_t busPins[] = {kSda, kScl};
    for (uint32_t gpio : busPins) {
        const uint32_t cfg = tlmm.read(tlmmCfg(gpio));
        if (((cfg >> 2) & 15) != kQupFunc) tlmm.write(tlmmCfg(gpio), (cfg & ~0x3ffu) | (kQupFunc << 2));
    }
    // Reset is active low on the Lumia, active high on the Z5C. If the firmware holds the controller
    // in reset, release it and give it the power-on delay the Linux driver uses (synaptics,pon-delay
    // = 66 ms, rounded up).
    const uint32_t cfg = tlmm.read(tlmmCfg(kReset));
    const bool driven = (tlmm.read(tlmmInOut(kReset)) >> 1) & 1;
    if ((cfg & (1u << 9)) && driven == kResetActiveHigh) {
        tlmm.write(tlmmInOut(kReset), kResetActiveHigh ? 0 : 1u << 1);
        IOLog("NativeTouch: released touch reset (gpio %u was driven %s)\n", kReset, driven ? "high" : "low");
        IOSleep(100);
    }
}

// --- QUP I2C, FIFO mode, polled ---

bool setState(uint32_t state)
{
    if (!poll(qup, kState, kStateValid, kStateValid, 20000)) return false;
    qup.write(kState, state);
    return poll(qup, kState, kStateValid | kStateMask, kStateValid | state, 20000);
}

bool qupInit()
{
    IOLog("NativeTouch: qup hw version 0x%08x, state 0x%08x, io modes 0x%08x, i2c status 0x%08x\n",
          qup.read(kHwVersion), qup.read(kState), qup.read(kIoModes), qup.read(kI2cStatus));
    qup.write(kSwReset, 1);
    if (!poll(qup, kState, kStateValid | kStateMask, kStateValid | kStateReset, 20000) || !setState(kStateReset)) {
        IOLog("NativeTouch: qup does not reach the reset state (0x%08x)\n", qup.read(kState));
        return false;
    }
    qup.write(kConfig, kConfigI2c);
    qup.write(kErrorFlagsEn, 0x3c);
    qup.write(kOperationalMask, kServiceFlags);
    qup.write(kMasterConfig, 1);   // tags v2
    if (!(qup.read(kState) & kMastGen)) {
        IOLog("NativeTouch: qup reports no I2C master (state 0x%08x)\n", qup.read(kState));
        return false;
    }
    return true;
}

uint32_t gClock = kClk100k;   // 400 kHz once the controller has answered at 100 kHz

// Write `reg`, repeated start, read `count` bytes (count + 2 tag bytes must fit the 64-byte FIFO).
bool readRegister(uint8_t reg, uint8_t *out, uint32_t count, uint32_t &status)
{
    status = 0;
    if (count == 0 || count > kMaxRead) return false;
    const uint8_t tx[] = {kTagStart, uint8_t(kTouchAddress << 1), kTagWrite, 1, reg,
                          kTagStart, uint8_t((kTouchAddress << 1) | 1), kTagReadStop, uint8_t(count)};
    if (!setState(kStateReset)) return false;
    qup.write(kMxInputCount, 0);
    qup.write(kMxOutputCount, 0);
    qup.write(kMxReadCount, count + 2);
    qup.write(kMxWriteCount, sizeof tx);
    qup.write(kConfig, (qup.read(kConfig) & ~(kNoInput | kNoOutput | 0xf1fu)) | kConfigI2c);
    qup.write(kIoModes, (qup.read(kIoModes) & ~(kModeBits | kPackUnpack)) | kPackUnpack);
    qup.write(kOperationalMask, 0);
    if (!setState(kStateRun)) return false;
    qup.write(kMasterClkCtl, gClock);
    if (!setState(kStatePause)) return false;
    for (uint32_t i = 0; i < sizeof tx; i += 4) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4 && i + b < sizeof tx; ++b) word |= uint32_t(tx[i + b]) << (8 * b);
        qup.write(kOutFifo, word);
    }
    if (!setState(kStateRun)) return false;
    bool done = false;
    for (int i = 0; i < 5000 && !done; ++i) {   // 50 ms; a 100 kHz read of 60 bytes takes ~7 ms
        status = qup.read(kI2cStatus);
        if (status & kStatusErrors) break;
        done = qup.read(kOperational) & kMaxInputDone;
        if (!done) IODelay(10);
    }
    if (done) {
        uint8_t in[64] = {};
        for (uint32_t i = 0; i < count + 2; i += 4) {
            const uint32_t word = qup.read(kInFifo);
            for (uint32_t b = 0; b < 4 && i + b < sizeof in; ++b) in[i + b] = uint8_t(word >> (8 * b));
        }
        for (uint32_t i = 0; i < count; ++i) out[i] = in[i + 2];
    }
    const uint32_t errors = qup.read(kErrorFlags);
    qup.write(kI2cStatus, status & kStatusErrors);
    qup.write(kErrorFlags, errors & 0x3c);
    qup.write(kOperational, qup.read(kOperational) & kServiceFlags);
    setState(kStateReset);
    return done && !(status & kStatusErrors);
}

// Write one byte to `reg` (output only: the QUP runs with no input phase).
bool writeRegister(uint8_t reg, uint8_t value, uint32_t &status)
{
    status = 0;
    const uint8_t tx[] = {kTagStart, uint8_t(kTouchAddress << 1), kTagWriteStop, 2, reg, value};
    if (!setState(kStateReset)) return false;
    qup.write(kMxInputCount, 0);
    qup.write(kMxOutputCount, 0);
    qup.write(kMxReadCount, 0);
    qup.write(kMxWriteCount, sizeof tx);
    qup.write(kConfig, (qup.read(kConfig) & ~(kNoInput | kNoOutput | 0xf1fu)) | kConfigI2c | kNoInput);
    qup.write(kIoModes, (qup.read(kIoModes) & ~(kModeBits | kPackUnpack)) | kPackUnpack);
    qup.write(kOperationalMask, 0);
    if (!setState(kStateRun)) return false;
    qup.write(kMasterClkCtl, gClock);
    if (!setState(kStatePause)) return false;
    for (uint32_t i = 0; i < sizeof tx; i += 4) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4 && i + b < sizeof tx; ++b) word |= uint32_t(tx[i + b]) << (8 * b);
        qup.write(kOutFifo, word);
    }
    if (!setState(kStateRun)) return false;
    bool done = false;
    for (int i = 0; i < 5000 && !done; ++i) {
        status = qup.read(kI2cStatus);
        if (status & kStatusErrors) break;
        // MAX_OUTPUT_DONE can lag the bus (i2c-msm-v2.c); idle bus with an empty FIFO also ends it.
        done = (qup.read(kOperational) & kMaxOutputDone) && !(status & (1u << 8));
        if (!done) IODelay(10);
    }
    const uint32_t errors = qup.read(kErrorFlags);
    qup.write(kI2cStatus, status & kStatusErrors);
    qup.write(kErrorFlags, errors & 0x3c);
    qup.write(kOperational, qup.read(kOperational) & kServiceFlags);
    qup.write(kConfig, qup.read(kConfig) & ~kNoInput);
    setState(kStateReset);
    return done && !(status & kStatusErrors);
}

struct Rmi4Function { uint8_t query, command, control, data, irqShift, irqCount; bool found; };
Rmi4Function gF01, gF12;

void probeRmi4()
{
    uint8_t irqShift = 0;
    // RMI4 page description table: 6-byte entries from 0xe9 down (query, command, control, data base,
    // interrupt sources / version, function number); an entry with function 0 ends the page.
    for (uint8_t entry = 0xe9; entry >= 0x0a; entry -= 6) {
        uint8_t pdt[6] = {};
        uint32_t status = 0;
        if (!readRegister(entry, pdt, sizeof pdt, status)) {
            IOLog("NativeTouch: pdt 0x%02x read failed, i2c status 0x%08x (%s%s%s), sda %u scl %u\n", entry, status,
                  (status & kNacked) ? "nack " : "", (status & (1u << 2)) ? "bus error " : "",
                  (status & (1u << 4)) ? "arbitration lost" : "", (status >> 26) & 1, (status >> 27) & 1);
            return;
        }
        IOLog("NativeTouch: pdt 0x%02x: function 0x%02x query 0x%02x command 0x%02x control 0x%02x data 0x%02x "
              "info 0x%02x\n", entry, pdt[5], pdt[0], pdt[1], pdt[2], pdt[3], pdt[4]);
        if (pdt[5] == 0 || pdt[5] == 0xff) break;
        const Rmi4Function function = {pdt[0], pdt[1], pdt[2], pdt[3], irqShift, uint8_t(pdt[4] & 7), true};
        irqShift = uint8_t(irqShift + (pdt[4] & 7));   // interrupt status bits are handed out in PDT order
        if (pdt[5] == 0x12) gF12 = function;
        if (pdt[5] == 0x01) {
            gF01 = function;
            uint8_t product[10] = {};
            if (readRegister(uint8_t(pdt[0] + 11), product, sizeof product, status)) {
                char id[11] = {};
                for (int i = 0; i < 10; ++i) id[i] = (product[i] >= 32 && product[i] < 127) ? char(product[i]) : 0;
                IOLog("NativeTouch: F01 product id '%s'\n", id);
            }
        }
    }
}

// --- reporting: RMI4 F12 finger objects -> NativeHID ---

extern "C" void native_hid_touch(int finger, int down, int x, int y) __attribute__((weak));

constexpr uint32_t kFingers = 10, kObjectBytes = 8;   // synaptics,num-of-fingers; F12 data1 object size
constexpr uint32_t kPanelW = 1440, kPanelH = 2560;    // synaptics,display-coords 0 0 1439 2559

struct F12Layout { uint8_t fingers; uint16_t maxX, maxY; bool interrupts; uint8_t irqMask; };

uint32_t popcount8(uint8_t v) { uint32_t n = 0; for (; v; v &= uint8_t(v - 1)) ++n; return n; }

// F12's registers are described by presence maps (synaptics_i2c_rmi4.c, synaptics_rmi4_f12_init):
// query 5 = size + control presence bits, query 8 = size + data presence bits. Data 1 (the objects)
// follows data 0 if that exists; control 8 holds the sensor's maximum X/Y.
bool f12Layout(F12Layout &layout)
{
    uint8_t q5[5] = {}, q8[3] = {}, ctrl8[4] = {}, f01ctrl[2] = {}, f01status = 0;
    uint32_t status = 0;
    if (!readRegister(uint8_t(gF12.query + 5), q5, sizeof q5, status) ||
        !readRegister(uint8_t(gF12.query + 8), q8, sizeof q8, status)) {
        IOLog("NativeTouch: F12 presence maps unreadable (i2c status 0x%08x)\n", status);
        return false;
    }
    const uint8_t ctrl8Offset = uint8_t(popcount8(q5[1]));
    if ((q5[2] & 1) && readRegister(uint8_t(gF12.control + ctrl8Offset), ctrl8, sizeof ctrl8, status)) {
        layout.maxX = uint16_t(ctrl8[0] | (ctrl8[1] << 8));
        layout.maxY = uint16_t(ctrl8[2] | (ctrl8[3] << 8));
    }
    if (layout.maxX == 0 || layout.maxY == 0) {
        layout.maxX = kPanelW - 1;
        layout.maxY = kPanelH - 1;
    }
    gF12.data = uint8_t(gF12.data + (q8[1] & 1));   // skip data 0
    layout.fingers = kFingers;
    layout.irqMask = uint8_t(((1u << gF12.irqCount) - 1) << gF12.irqShift);
    readRegister(gF01.data, &f01status, 1, status);
    readRegister(gF01.control, f01ctrl, sizeof f01ctrl, status);
    layout.interrupts = (f01ctrl[1] & layout.irqMask) == layout.irqMask;
    IOLog("NativeTouch: F12 q5 %02x %02x %02x %02x %02x, q8 %02x %02x %02x, objects at 0x%02x, sensor max %u x %u; "
          "F01 status 0x%02x ctrl0 0x%02x (sleep %u, nosleep %u) irq enable 0x%02x (F12 mask 0x%02x %s)\n",
          q5[0], q5[1], q5[2], q5[3], q5[4], q8[0], q8[1], q8[2], gF12.data, layout.maxX, layout.maxY, f01status,
          f01ctrl[0], f01ctrl[0] & 3, (f01ctrl[0] >> 2) & 1, f01ctrl[1], layout.irqMask,
          layout.interrupts ? "enabled" : "not enabled, polling objects");
    if (f01ctrl[0] & 3) {   // sleeping: back to normal operation
        writeRegister(gF01.control, uint8_t(f01ctrl[0] & ~3u), status);
        IOLog("NativeTouch: F01 woken from sleep mode %u (i2c status 0x%08x)\n", f01ctrl[0] & 3, status);
    }
    return true;
}

void report(const F12Layout &layout)
{
    static bool down[kFingers];
    static int lastX[kFingers], lastY[kFingers];   // a lifted object reads as 0,0: keep where it was
    static uint32_t logged;
    uint8_t objects[kFingers * kObjectBytes] = {};
    uint32_t status = 0;
    // Two reads: the input FIFO holds 60 bytes, ten objects are 80.
    if (!readRegister(gF12.data, objects, 7 * kObjectBytes, status) ||
        !readRegister(uint8_t(gF12.data + 7 * kObjectBytes), objects + 7 * kObjectBytes,
                      (kFingers - 7) * kObjectBytes, status)) {
        if (logged++ < 20) IOLog("NativeTouch: object read failed (i2c status 0x%08x)\n", status);
        return;
    }
    for (uint32_t i = 0; i < layout.fingers; ++i) {
        const uint8_t *o = objects + i * kObjectBytes;
        // object type: 0 none, 1 finger, 2 stylus, 3 palm, 5 glove, 6 hover finger (F12 data 1)
        const bool touching = o[0] == 1 || o[0] == 2 || o[0] == 5;
        if (!touching && !down[i]) continue;
        const uint32_t rawX = uint32_t(o[1] | (o[2] << 8)), rawY = uint32_t(o[3] | (o[4] << 8));
        const int x = int(rawX * kPanelW / (uint32_t(layout.maxX) + 1));
        const int y = int(rawY * kPanelH / (uint32_t(layout.maxY) + 1));
        if (touching && !down[i] && logged++ < 40) {
            IOLog("NativeTouch: finger %u type %u raw %u,%u -> panel %d,%d z %u\n", i, o[0], rawX, rawY, x, y, o[5]);
        }
        down[i] = touching;
        if (touching) {
            lastX[i] = x;
            lastY[i] = y;
        }
        if (native_hid_touch) native_hid_touch(int(i), touching, lastX[i], lastY[i]);
    }
}

// --- side keys: PM8994 PON status and GPIOs -> NativeHID ---

extern "C" void native_hid_key(int key, int down) __attribute__((weak));

constexpr uint16_t kPonRtStatus = 0x0810;              // bit 0 KPDPWR (power), bit 1 RESIN (volume down)
constexpr uint16_t pmGpio(uint32_t n) { return uint16_t(0xc000 + 0x100 * (n - 1)); }
constexpr uint16_t kGpioStatus = 0x08, kGpioMode = 0x40, kGpioPull = 0x42, kGpioEnable = 0x46;

struct SideKey { const char *name; int hidKey; bool down; uint8_t pending; };
SideKey gKeys[] = {{"power", 0, false, 0}, {"volume up", 1, false, 0}, {"volume down", 2, false, 0},
                   {"camera (home)", 3, false, 0}};
bool gKeysUsable;

void logKeyPin(const char *name, uint32_t n)
{
    uint8_t status = 0, mode = 0, pull = 0, enable = 0;
    if (!spmiRead(0, pmGpio(n) + kGpioStatus, status) || !spmiRead(0, pmGpio(n) + kGpioMode, mode) ||
        !spmiRead(0, pmGpio(n) + kGpioPull, pull) || !spmiRead(0, pmGpio(n) + kGpioEnable, enable)) {
        IOLog("NativeTouch: pm8994 gpio %u (%s): SPMI read failed\n", n, name);
        return;
    }
    IOLog("NativeTouch: pm8994 gpio %u (%s): status 0x%02x mode 0x%02x pull 0x%02x enable 0x%02x\n", n, name,
          status, mode, pull, enable);
}

void logKeys()
{
    uint8_t pon = 0;
    gKeysUsable = spmiRead(0, kPonRtStatus, pon);
    if (!gKeysUsable) {
        IOLog("NativeTouch: side keys: PON status unreadable, no keys\n");
        return;
    }
    IOLog("NativeTouch: side keys: PON status 0x%02x (%s)\n", pon,
          native_hid_key ? "to NativeHID" : "NativeHID not linked: log only");
    logKeyPin("volume up", 3);
    logKeyPin("camera", 4);
}

void pollKeys()
{
    uint8_t pon = 0, up = 0, camera = 0;
    if (!gKeysUsable || !spmiRead(0, kPonRtStatus, pon) || !spmiRead(0, pmGpio(3) + kGpioStatus, up) ||
        !spmiRead(0, pmGpio(4) + kGpioStatus, camera)) {
        return;
    }
    const bool now[] = {(pon & 1) != 0, (up & 1) == 0, (pon & 2) != 0, (camera & 1) == 0};
    for (uint32_t i = 0; i < sizeof gKeys / sizeof gKeys[0]; ++i) {
        SideKey &key = gKeys[i];
        key.pending = now[i] != key.down ? uint8_t(key.pending + 1) : 0;
        if (key.pending < 2) continue;
        key.down = now[i];
        key.pending = 0;
        IOLog("NativeTouch: %s %s\n", key.name, key.down ? "down" : "up");
        if (native_hid_key) native_hid_key(key.hidKey, key.down);
    }
}

void pollLoop(const F12Layout *layout)
{
    IOLog("NativeTouch: polling%s side keys (%s)\n", layout ? " touches and" : "",
          native_hid_touch ? "to NativeHID" : "NativeHID not linked: log only");
    for (;;) {
        uint32_t status = 0;
        uint8_t irq = 0;
        if (layout && layout->interrupts) {
            // Reading F01's interrupt status clears it (and the attention line); F12's bit says new objects.
            if (readRegister(uint8_t(gF01.data + 1), &irq, 1, status) && (irq & layout->irqMask)) report(*layout);
        } else if (layout) {
            report(*layout);
        }
        pollKeys();
        IOSleep(12);
    }
}

bool probeTouch(F12Layout &layout)
{
    if (!gcc.open(kGccPhys, kGccSize) || !tlmm.open(kTlmmPhys, kTlmmSize) || !qup.open(kQupPhys, kQupSize)) {
        IOLog("NativeTouch: a register window did not map\n");
        return false;
    }
    checkpoint("reading touch supplies over SPMI");
    logSupply("L22 (touch vdd 3.0 V)", 0x5500);
    logSupply("LVS1 (touch i2c 1.8 V)", 0x8000);
    logPin("sda", kSda);
    logPin("scl", kScl);
    logPin("reset", kReset);
    logPin("irq", kIrq);
    checkpoint("checking the BLSP1 / QUP1 clocks");
    if (!clocksOn()) return false;
    pinsForBus();
    logPin("sda", kSda);
    logPin("scl", kScl);
    logPin("reset", kReset);
    checkpoint("first QUP1 register access");
    if (!qupInit()) return false;
    checkpoint("reading the RMI4 page description table at 0x4b");
    probeRmi4();
    logPin("irq", kIrq);
    IOLog("NativeTouch: probe done\n");
    if (!gF01.found || !gF12.found) {
        IOLog("NativeTouch: no F01/F12, no touch reporting\n");
        return false;
    }
    gClock = kClk400k;
    return f12Layout(layout);
}

void probeThread(void *, wait_result_t)
{
    IOSleep(30000);   // after the root mount and the console log's first flushes
    uint8_t type = 0;
    if (!spmiRead(1, 0x5504, type)) {
        IOLog("NativeTouch: no SPMI reads (window or channel)\n");
        return;
    }
    F12Layout layout = {};
    const bool touch = probeTouch(layout);
    logKeys();
    pollLoop(touch ? &layout : nullptr);
}
}

extern "C" bool native_spmi_read(uint8_t slave, uint16_t address, uint8_t *value)
{
    return spmiRead(slave, address, *value);
}

class NativeTouch final : public IOService {
    OSDeclareDefaultStructors(NativeTouch)
public:
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider)) return false;
        gSpmiLock = IOLockAlloc();
        uint32_t enabled = 1;
        PE_parse_boot_argn("nativetouch", &enabled, sizeof enabled);
        if (!enabled) {
            IOLog("NativeTouch: probe disabled (nativetouch=0)\n");
            return true;
        }
        thread_t thread = nullptr;
        if (kernel_thread_start(probeThread, nullptr, &thread) != KERN_SUCCESS) {
            IOLog("NativeTouch: no probe thread\n");
            return true;
        }
        thread_deallocate(thread);
        IOLog("NativeTouch: probe starts in 30 s\n");
        return true;
    }
};
OSDefineMetaClassAndStructors(NativeTouch, IOService)
