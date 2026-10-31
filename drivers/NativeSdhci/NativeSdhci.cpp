// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSdhci: the Lumia 950 (MSM8992) eMMC driver.
//
// What this driver does today: the firmware's card does not answer after ExitBootServices, so the
// card is identified again from CMD0 (400 kHz through the GCC SDCC1 clock, then 25 MHz), moved to the
// 8-bit bus and high-speed timing at 50 MHz when EXT_CSD proves each step, and served by programmed
// I/O with every wait polled: up to 128 blocks per CMD18/CMD25 with Auto CMD12. Writes are confined
// to the GPT partition named "ios" and need boot-arg sdhcirw=1; elsewhere the medium is read-only.
//
// Reads go by ADMA2 once a self-test at probe has matched them against PIO (setupDma; boot-arg
// sdhcidma=0 keeps PIO): at PIO's ~10 MB/s the 4K code signature pass spent ~280 s of every boot
// reading the 2.7 GB shared cache (boot 17). Writes stay PIO.
//
// What it does not do yet, in the order the bring-up ladder adds it:
//   * interrupt delivery (hc_irq is a GICv2 SPI; ArmCpu's GIC path is the gate)
//   * ADMA2 writes
//   * the eMMC cache (left off, so every write is on the medium when it completes)
//   * HS200/HS400 (QSDCC vendor registers and tuning; see README.md, "Vendor registers")
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/thread.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

#include "NativeSdhci.h"
#include "SdhciCore.h"
#include "SdhciRegs.h"
#include "BoardConstants.h"

using namespace sdhci;

OSDefineMetaClassAndStructors(NativeSdhci, IOBlockStorageDevice)

// Published for the bring-up log and the kernel symbol table, in the style of
// the other native drivers: a boot with no serial console still shows whether
// the driver started, how many blocks it moved and where it gave up.
extern "C" {
volatile uint32_t native_sdhci_ready  = 0;
volatile uint32_t native_sdhci_stage  = 0;   // last completed bring-up step
volatile uint32_t native_sdhci_rca    = 0;
volatile uint64_t native_sdhci_bytes  = 0;
volatile uint64_t native_sdhci_reads  = 0;
volatile uint64_t native_sdhci_writes = 0;
volatile uint64_t native_sdhci_errors = 0;
volatile uint32_t native_sdhci_error_phase = 0;
volatile uint32_t native_sdhci_interrupt_status = 0;
volatile uint32_t native_sdhci_card_status = 0;
}

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------

uint8_t NativeSdhci::readHc8(uint32_t offset) const
{
    if (!hc || offset >= lumia950::kSdhciHcSize) return 0xff;
    asm volatile("dsb sy" ::: "memory");
    const uint8_t value = hc[offset];
    asm volatile("dsb sy" ::: "memory");
    return value;
}

uint16_t NativeSdhci::readHc16(uint32_t offset) const
{
    if (!hc || (offset & 1) || offset > lumia950::kSdhciHcSize - 2) return 0xffff;
    asm volatile("dsb sy" ::: "memory");
    const uint16_t value = *reinterpret_cast<volatile uint16_t *>(hc + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

uint32_t NativeSdhci::readHc32(uint32_t offset) const
{
    if (!hc || (offset & 3) || offset > lumia950::kSdhciHcSize - 4) return 0xffffffffu;
    asm volatile("dsb sy" ::: "memory");
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(hc + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

void NativeSdhci::writeHc8(uint32_t offset, uint8_t value)
{
    if (!hc || offset >= lumia950::kSdhciHcSize) return;
    asm volatile("dsb sy" ::: "memory");
    hc[offset] = value;
    asm volatile("dsb sy" ::: "memory");
}

void NativeSdhci::writeHc16(uint32_t offset, uint16_t value)
{
    if (!hc || (offset & 1) || offset > lumia950::kSdhciHcSize - 2) return;
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint16_t *>(hc + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

void NativeSdhci::writeHc32(uint32_t offset, uint32_t value)
{
    if (!hc || (offset & 3) || offset > lumia950::kSdhciHcSize - 4) return;
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(hc + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

uint32_t NativeSdhci::readCore32(uint32_t offset) const
{
    if (!core || (offset & 3) || offset > lumia950::kSdhciCoreSize - 4) return 0xffffffffu;
    asm volatile("dsb sy" ::: "memory");
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(core + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

void NativeSdhci::writeCore32(uint32_t offset, uint32_t value)
{
    if (!core || (offset & 3) || offset > lumia950::kSdhciCoreSize - 4) return;
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(core + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

uint32_t NativeSdhci::readGcc32(uint32_t offset) const
{
    if (!gcc || (offset & 3) || offset > lumia950::kGccSize - 4) return 0xffffffffu;
    asm volatile("dsb sy" ::: "memory");
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(gcc + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

void NativeSdhci::writeGcc32(uint32_t offset, uint32_t value)
{
    if (!gcc || (offset & 3) || offset > lumia950::kGccSize - 4) return;
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(gcc + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

u8 NativeSdhci::ioRead8(void *context, u32 offset)
{
    return static_cast<NativeSdhci *>(context)->readHc8(offset);
}

u32 NativeSdhci::ioRead32(void *context, u32 offset)
{
    return static_cast<NativeSdhci *>(context)->readHc32(offset);
}

void NativeSdhci::ioWrite8(void *context, u32 offset, u8 value)
{
    static_cast<NativeSdhci *>(context)->writeHc8(offset, value);
}

void NativeSdhci::ioWrite16(void *context, u32 offset, u16 value)
{
    static_cast<NativeSdhci *>(context)->writeHc16(offset, value);
}

void NativeSdhci::ioWrite32(void *context, u32 offset, u32 value)
{
    static_cast<NativeSdhci *>(context)->writeHc32(offset, value);
}

void NativeSdhci::ioDelay(void *, u32 microseconds)
{
    IODelay(microseconds);
}

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

bool NativeSdhci::mapWindows(IOService *provider)
{
    // IOPlatformDevice publishes the ADT node's reg entries as device memory,
    // in order: 0 is the SDHCI window, 1 the Qualcomm core window.
    hcMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    coreMap = provider->mapDeviceMemoryWithIndex(1, kIOMapInhibitCache);
    if (!hcMap || !coreMap) {
        IOLog("NativeSdhci: provider has no reg windows (hc %p core %p); "
              "the ADT node needs both reg entries\n", hcMap, coreMap);
        return false;
    }
    if (hcMap->getLength() < lumia950::kSdhciHcSize) {
        IOLog("NativeSdhci: hc window is %llu bytes, expected at least 0x%x\n",
              static_cast<unsigned long long>(hcMap->getLength()), lumia950::kSdhciHcSize);
        return false;
    }

    if (coreMap->getLength() < lumia950::kSdhciCoreSize) {
        IOLog("NativeSdhci: core window is too short\n");
        return false;
    }

    // The clock controller is not part of the ADT node: map just the page that holds
    // SDCC1's clock registers. Without it the driver can still try the warm handoff.
    IOMemoryDescriptor *gccRange = IOMemoryDescriptor::withPhysicalAddress(
        lumia950::kGccPhys, lumia950::kGccSize, kIODirectionInOut);
    if (gccRange) {
        gccMap = gccRange->map(kIOMapInhibitCache);
        gccRange->release();
    }
    if (gccMap) gcc = reinterpret_cast<volatile uint8_t *>(gccMap->getVirtualAddress());
    else IOLog("NativeSdhci: GCC window did not map; no cold identification\n");

    hc = reinterpret_cast<volatile uint8_t *>(hcMap->getVirtualAddress());
    core = reinterpret_cast<volatile uint8_t *>(coreMap->getVirtualAddress());
    if (!hc || !core) {
        IOLog("NativeSdhci: window mapping returned no virtual address\n");
        return false;
    }
    return true;
}

void NativeSdhci::logHost()
{
    hostVersion = readHc16(kRegHostVersion);
    const uint32_t capabilities = readHc32(kRegCapabilities);
    baseClockHz = ((capabilities >> kCapBaseClockShift) & kCapBaseClockMask) * 1000000u;

    const uint16_t clockControl = readHc16(kRegClockControl);
    const uint32_t divider = decodeDivider(clockControl);
    const uint32_t cardHz = baseClockHz && divider ? baseClockHz / (2 * divider) : 0;

    IOLog("NativeSdhci: host version 0x%04x, capabilities 0x%08x\n", hostVersion, capabilities);
    IOLog("NativeSdhci: base clock %u Hz, clock control 0x%04x (divider %u, card %u Hz, enabled %u)\n",
          baseClockHz, clockControl, divider, cardHz, (clockControl & kClockSdClockEnable) ? 1 : 0);
    IOLog("NativeSdhci: present state 0x%08x, host control 0x%02x, power 0x%02x, "
          "core[0x00] 0x%08x\n",
          readHc32(kRegPresentState), readHc8(kRegHostControl1), readHc8(kRegPowerControl),
          readCore32(0x00));
}

// The state the firmware handed over, read before the first reset: the QSDCC vendor
// registers (offsets from the cityman kernel's sdhci-msm.c) and SDCC1's GCC clocks.
void NativeSdhci::logVendor()
{
    IOLog("NativeSdhci: host control2 0x%04x, int status 0x%08x enable 0x%08x signal 0x%08x\n",
          readHc16(kRegHostControl2), readHc32(kRegInterruptStatus),
          readHc32(kRegInterruptEnable), readHc32(kRegInterruptSignal));
    IOLog("NativeSdhci: core mci 0x%08x mode 0x%08x pwrctl status 0x%08x mask 0x%08x ctl 0x%08x\n",
          readCore32(0x50), readCore32(0x78), readCore32(0xdc), readCore32(0xe0), readCore32(0xe8));
    IOLog("NativeSdhci: core dll 0x%08x status 0x%08x vendor 0x%08x func2 0x%08x spec3 0x%08x\n",
          readCore32(0x100), readCore32(0x108), readCore32(0x10c), readCore32(0x110), readCore32(0x1b0));
    const uint32_t rcg = lumia950::kGccSdcc1AppsCmdRcgr;
    IOLog("NativeSdhci: gcc apps cbcr 0x%08x ahb cbcr 0x%08x rcg cmd 0x%08x cfg 0x%08x m 0x%08x n 0x%08x d 0x%08x\n",
          readGcc32(lumia950::kGccSdcc1AppsCbcr), readGcc32(lumia950::kGccSdcc1AhbCbcr),
          readGcc32(rcg), readGcc32(rcg + 4), readGcc32(rcg + 8), readGcc32(rcg + 0xc),
          readGcc32(rcg + 0x10));
}

// Program the card clock for legacy timing: 1-bit bus, no HS, UHS mode 0 (feedback
// clock), DLL off, default MCLK, then the GCC root at `hz`. The MSM host does not use
// the SDHCI divider (sdhci-msm writes 0 and only toggles the enables), so the rate is
// whatever the RCG produces.
bool NativeSdhci::setBusClock(uint32_t hz)
{
    struct Rate { uint32_t hz, cfg, m, n, d; };
    // cfg = divider (2*div - 1) | source << 8 | dual-edge M/N mode << 12
    static const Rate rates[] = {
        {   400000, 23 | (0u << 8) | (2u << 12), 1, ~3u, ~4u },   // XO / 12 * 1/4
        { 25000000, 23 | (1u << 8) | (2u << 12), 1, ~1u, ~2u },   // GPLL0 / 12 * 1/2
        { 50000000, 23 | (1u << 8),              0,  0u, ~0u },   // GPLL0 / 12, M/N bypassed
    };
    const Rate *rate = nullptr;
    for (const Rate &candidate : rates) if (candidate.hz == hz) rate = &candidate;
    if (!rate || !gcc) return false;

    writeHc16(kRegClockControl, readHc16(kRegClockControl) & ~kClockSdClockEnable);
    // High-speed timing (output on the rising edge) above the 26 MHz legacy limit.
    const uint8_t host1 = readHc8(kRegHostControl1) & ~kHostHighSpeed;
    writeHc8(kRegHostControl1, hz > 26000000 ? host1 | kHostHighSpeed : host1);
    writeHc16(kRegHostControl2, readHc16(kRegHostControl2) & ~7u);

    // VENDOR_SPEC: default MCLK, HC_SELECT_IN off, no clock power save while identifying.
    uint32_t vendor = readCore32(0x10c);
    vendor &= ~((3u << 8) | (1u << 18) | (7u << 19) | (1u << 1));
    vendor |= 2u << 8;
    writeCore32(0x10c, vendor);
    writeCore32(0x1b0, readCore32(0x1b0) & ~(1u << 3));
    writeCore32(0x100, readCore32(0x100) | (1u << 30));
    writeCore32(0x100, readCore32(0x100) | (1u << 29));

    const uint32_t rcg = lumia950::kGccSdcc1AppsCmdRcgr;
    writeGcc32(rcg + 8, rate->m);
    writeGcc32(rcg + 0xc, rate->n);
    writeGcc32(rcg + 0x10, rate->d);
    writeGcc32(rcg + 4, (readGcc32(rcg + 4) & ~(0x1fu | (7u << 8) | (3u << 12))) | rate->cfg);
    writeGcc32(rcg, readGcc32(rcg) | 1);
    uint32_t polls = 0;
    while ((readGcc32(rcg) & 1) && ++polls < 500) IODelay(1);
    writeGcc32(lumia950::kGccSdcc1AppsCbcr, readGcc32(lumia950::kGccSdcc1AppsCbcr) | 1);
    uint32_t branch = 0;
    while ((readGcc32(lumia950::kGccSdcc1AppsCbcr) & (1u << 31)) && ++branch < 500) IODelay(1);

    writeHc16(kRegClockControl, kClockInternalEnable);
    uint32_t stable = 0;
    while (!(readHc16(kRegClockControl) & kClockInternalStable) && ++stable < 20000) IODelay(1);
    writeHc16(kRegClockControl, kClockInternalEnable | kClockSdClockEnable);
    IODelay(1000);

    IOLog("NativeSdhci: bus clock %u Hz: rcg cmd 0x%08x cfg 0x%08x (update %u polls), "
          "cbcr 0x%08x, clock control 0x%04x\n",
          hz, readGcc32(rcg), readGcc32(rcg + 4), polls,
          readGcc32(lumia950::kGccSdcc1AppsCbcr), readHc16(kRegClockControl));
    return polls < 500 && branch < 500 && stable < 20000;
}

bool NativeSdhci::busClock(void *context, u32 hz)
{
    return static_cast<NativeSdhci *>(context)->setBusClock(hz);
}

bool NativeSdhci::setBusWidth(uint32_t bits)
{
    if (bits != 1 && bits != 8) return false;
    const uint8_t host1 = readHc8(kRegHostControl1) & ~(kHostDataWidth4 | kHostDataWidth8);
    writeHc8(kRegHostControl1, bits == 8 ? host1 | kHostDataWidth8 : host1);
    return true;
}

bool NativeSdhci::busWidth(void *context, u32 bits)
{
    return static_cast<NativeSdhci *>(context)->setBusWidth(bits);
}

void NativeSdhci::logFailure()
{
    const HostResult &failure = host.result();
    native_sdhci_error_phase = static_cast<uint32_t>(failure.phase);
    native_sdhci_interrupt_status = failure.interruptStatus;
    native_sdhci_card_status = failure.cardStatus;
    __atomic_fetch_add(&native_sdhci_errors, 1, __ATOMIC_RELAXED);
    IOLog("NativeSdhci: %s, CMD%u, phase %u, interrupt 0x%08x, card 0x%08x, attach %u\n",
          hostErrorName(failure.error), failure.command, static_cast<unsigned>(failure.phase),
          failure.interruptStatus, failure.cardStatus, static_cast<unsigned>(card.error()));
    IOLog("NativeSdhci: present 0x%08x clock 0x%04x host1 0x%02x host2 0x%04x int 0x%08x/0x%08x "
          "vendor 0x%08x mci status 0x%08x\n",
          readHc32(kRegPresentState), readHc16(kRegClockControl), readHc8(kRegHostControl1),
          readHc16(kRegHostControl2), readHc32(kRegInterruptStatus), readHc32(kRegInterruptEnable),
          readCore32(0x10c), readCore32(0x34));
    for (u32 back = 4; back-- > 0;) {
        const CommandTrace *trace = host.recent(back);
        if (!trace) continue;
        IOLog("NativeSdhci:   CMD%u arg 0x%08x -> status 0x%08x response 0x%08x\n",
              trace->index, trace->argument, trace->status, trace->response);
    }
}

// The eMMC host is not cache coherent, and on this kernel IOMemoryDescriptor's incoherent-I/O
// operations can compile to a barrier only: clean and invalidate the lines by hand.
static void cleanInvalidate(const void *address, size_t length)
{
    uintptr_t line = reinterpret_cast<uintptr_t>(address) & ~static_cast<uintptr_t>(63);
    const uintptr_t end = reinterpret_cast<uintptr_t>(address) + length;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; line < end; line += 64) __asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool physicalBelow4G(IOBufferMemoryDescriptor *buffer, uint32_t &phys)
{
    IOByteCount length = 0;
    const addr64_t at = buffer->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    if (!at || length < buffer->getLength() || at + buffer->getLength() > 0x100000000ULL) return false;
    phys = static_cast<uint32_t>(at);
    return true;
}

// Buffers, then the self-test: the GPT area and a stretch of the 'ios' partition read by PIO and by
// ADMA2 (1, 8, kMaxTransferBlocks and kMaxDmaBlocks blocks) must agree byte for byte, or reads stay PIO.
void NativeSdhci::setupDma()
{
    uint32_t enabled = 1;
    PE_parse_boot_argn("sdhcidma", &enabled, sizeof enabled);
    if (!enabled) {
        IOLog("NativeSdhci: ADMA2 off (sdhcidma=0), reads by PIO\n");
        return;
    }
    const uint32_t bytes = kMaxDmaBlocks * lumia950::kLogicalBlockBytes;
    dmaBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut |
        kIOMemoryPhysicallyContiguous, bytes, 0x00000000fffff000ULL);
    dmaTable = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut |
        kIOMemoryPhysicallyContiguous, 4096, 0x00000000fffff000ULL);
    if (!dmaBuffer || !dmaTable || !physicalBelow4G(dmaBuffer, dmaBufferPhys) ||
        !physicalBelow4G(dmaTable, dmaTablePhys)) {
        IOLog("NativeSdhci: no DMA buffers below 4 GiB, reads by PIO\n");
        return;
    }
    const uint64_t probes[] = {0, windowFirst ? windowFirst : 34, windowFirst ? windowFirst + 4096 : 2048};
    const uint32_t counts[] = {1, 8, kMaxTransferBlocks, kMaxDmaBlocks};
    dma = true;
    for (uint64_t lba : probes) {
        for (uint32_t count : counts) {
            memset(dmaBuffer->getBytesNoCopy(), 0xa5, bytes);
            bool same = readBlocksDma(lba, count);
            // PIO moves at most kMaxTransferBlocks at a time: compare in pieces of that size.
            for (uint32_t at = 0; same && at < count; at += kMaxTransferBlocks) {
                const uint32_t piece = count - at < kMaxTransferBlocks ? count - at : kMaxTransferBlocks;
                same = readBlocksPio(lba + at, piece, bounce) &&
                    memcmp(bounce, static_cast<const uint8_t *>(dmaBuffer->getBytesNoCopy()) +
                           at * lumia950::kLogicalBlockBytes, piece * lumia950::kLogicalBlockBytes) == 0;
            }
            if (!same) {
                IOLog("NativeSdhci: ADMA2 self-test failed at LBA %llu +%u (host error %u phase %u status 0x%08x, "
                      "adma error 0x%02x), reads by PIO\n", static_cast<unsigned long long>(lba), count,
                      static_cast<unsigned>(host.result().error), static_cast<unsigned>(host.result().phase),
                      readHc32(kRegInterruptStatus), readHc8(kRegAdmaErrorStatus));
                dma = false;
                card.recover();
                return;
            }
        }
    }
    IOLog("NativeSdhci: ADMA2 reads on (self-test matched PIO; buffer 0x%08x, table 0x%08x)\n", dmaBufferPhys,
          dmaTablePhys);
}

// count blocks from lba into dmaBuffer by ADMA2, in 32 KiB descriptors.
bool NativeSdhci::readBlocksDma(uint64_t lba, uint32_t count)
{
    const uint32_t length = count * lumia950::kLogicalBlockBytes;
    Adma2Chunk chunks[kMaxDmaBlocks * lumia950::kLogicalBlockBytes / 0x8000];
    uint32_t pieces = 0;
    for (uint32_t at = 0; at < length; at += 0x8000) {
        chunks[pieces++] = {dmaBufferPhys + at, length - at < 0x8000 ? length - at : 0x8000u};
    }
    u64 *table = static_cast<u64 *>(dmaTable->getBytesNoCopy());
    if (!buildAdma2(chunks, pieces, table, 4096 / 8)) return false;
    cleanInvalidate(table, pieces * 8);
    cleanInvalidate(dmaBuffer->getBytesNoCopy(), length);
    const bool ok = card.readBlocksDma(lba, count, dmaTablePhys);
    cleanInvalidate(dmaBuffer->getBytesNoCopy(), length);   // drop lines prefetched during the transfer
    return ok;
}

bool NativeSdhci::readBlocksPio(uint64_t lba, uint32_t count, void *bytes)
{
    if (card.readBlocks(lba, count, static_cast<u8 *>(bytes))) return true;
    online = false;
    __atomic_store_n(&native_sdhci_ready, 0, __ATOMIC_RELAXED);
    logFailure();
    return false;
}

// CMD0 identification at 400 kHz, then the 8-bit bus and high-speed timing. A card whose
// upgrade fails is identified again and kept on the legacy 1-bit bus.
bool NativeSdhci::identifyCold()
{
    const BusControl bus = { this, busClock, busWidth };
    if (!card.initialize(host, lumia950::kColdRelativeAddress, bus, lumia950::kTransferHz)) {
        logFailure();
        IOLog("NativeSdhci: cold identification failed (OCR 0x%08x)\n", card.info().ocr);
        return false;
    }
    IOLog("NativeSdhci: cold card OCR 0x%08x CID %08x %08x %08x %08x\n", card.info().ocr,
          card.info().cid[0], card.info().cid[1], card.info().cid[2], card.info().cid[3]);
    if (card.upgrade(bus, lumia950::kHighSpeedHz)) return true;
    logFailure();
    IOLog("NativeSdhci: bus upgrade failed; identifying again on the legacy bus\n");
    if (!card.initialize(host, lumia950::kColdRelativeAddress, bus, lumia950::kTransferHz)) {
        logFailure();
        return false;
    }
    return true;
}

// The write window: the GPT partition named kWritablePartition, and only with boot-arg sdhcirw=1.
void NativeSdhci::findWritableWindow()
{
    uint32_t enable = 0;
    const bool asked = PE_parse_boot_argn("sdhcirw", &enable, sizeof(enable)) && enable;
    // Header at LBA 1, then the 128-entry array the header points at (LBA 2..33 on this medium).
    alignas(8) uint8_t header[lumia950::kLogicalBlockBytes];
    sdhci::GptRange range = {};
    if (!readBlocksPio(1, 1, header)) return;
    const uint64_t entriesLba = static_cast<uint64_t>(header[72]) | (static_cast<uint64_t>(header[73]) << 8);
    if (entriesLba < 2 || entriesLba > 64 || !readBlocksPio(entriesLba, 32, bounce)) return;
    if (!sdhci::findGptPartition(header, bounce, 32 * lumia950::kLogicalBlockBytes, blockCount - 1,
                                 lumia950::kWritablePartition, &range)) {
        IOLog("NativeSdhci: no valid GPT partition '%s'; the medium stays read-only\n",
              lumia950::kWritablePartition);
        return;
    }
    windowFirst = range.first;
    windowLast = range.last;
    if (asked) card.allowWrites(range.first, range.last);
    IOLog("NativeSdhci: GPT '%s' LBA %llu-%llu, writes %s\n", lumia950::kWritablePartition,
          static_cast<unsigned long long>(range.first), static_cast<unsigned long long>(range.last),
          card.writable() ? "enabled there only" : "off (boot-arg sdhcirw=1 enables them)");
}

// ---------------------------------------------------------------------------
// Console log
//
// video_console.c keeps every character the console draws in native_console_ring, muted by the
// GUI or not. Once a second the new part is copied to a ring at the end of the writable
// partition, so a boot can be read back from Android afterwards:
//
//   header (1 block): "NATLOG01", u64 stream head, u32 ring blocks, u32 boot,
//                     u64 boot base, u64 bytes lost, u64 flushes (little-endian)
//   ring (kConsoleLogBlocks blocks): stream byte s at ring byte s % ring size
//
// Each boot continues the stream after the previous one, from a block boundary, and starts with
// one marker block naming the boot. A ring that would reach into the HFS+ volume is never used.
// ---------------------------------------------------------------------------

extern "C" {
extern char native_console_ring[];
extern const uint32_t native_console_ring_bytes;
extern volatile uint64_t native_console_ring_head;
}

namespace {

constexpr char kLogMagic[8] = { 'N', 'A', 'T', 'L', 'O', 'G', '0', '1' };

uint32_t be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint64_t le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}

void putLe64(uint8_t *p, uint64_t value)
{
    for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void putLe32(uint8_t *p, uint32_t value)
{
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

} // namespace

bool NativeSdhci::writeLogBlocks(uint64_t lba, uint32_t count, const uint8_t *bytes)
{
    if (logPanicking) return online && card.writeBlocks(lba, count, bytes);
    IOLockLock(gate);
    const bool ok = online && card.writeBlocks(lba, count, bytes);
    if (ok) __atomic_fetch_add(&native_sdhci_writes, 1, __ATOMIC_RELAXED);
    IOLockUnlock(gate);
    if (!ok && logging) {
        logging = false;
        IOLog("NativeSdhci: console log write at LBA %llu failed (%s); log stopped\n",
              static_cast<unsigned long long>(lba), hostErrorName(host.result().error));
    }
    return ok;
}

void NativeSdhci::writeLogHeader()
{
    alignas(8) uint8_t header[lumia950::kLogicalBlockBytes] = {};
    memcpy(header, kLogMagic, sizeof kLogMagic);
    putLe64(header + 8, logBootBase + lumia950::kLogicalBlockBytes + logFlushed);
    putLe32(header + 16, lumia950::kConsoleLogBlocks);
    putLe32(header + 20, logBoot);
    putLe64(header + 24, logBootBase);
    putLe64(header + 32, logLost);
    putLe64(header + 40, logFlushes);
    writeLogBlocks(logHeaderLba, 1, header);
}

void NativeSdhci::flushConsoleLog(bool panicking)
{
    logPanicking = panicking;
    if (panicking && !card.recover()) return;
    const uint64_t ringBytes = native_console_ring_bytes;
    const uint64_t head = __atomic_load_n(&native_console_ring_head, __ATOMIC_ACQUIRE);
    if (head == logFlushed) return;

    // The partial block written last time is written again with what has been added to it.
    uint64_t from = logFlushed & ~static_cast<uint64_t>(lumia950::kLogicalBlockBytes - 1);
    const uint64_t keep = ringBytes - 64 * 1024;   // margin for writes during the copy
    if (head - from > keep) {
        const uint64_t resume = (head - keep) & ~static_cast<uint64_t>(lumia950::kLogicalBlockBytes - 1);
        logLost += resume - from;
        from = resume;
    }

    const uint64_t ringBlocks = lumia950::kConsoleLogBlocks;
    while (from < head && logging) {
        // This boot's stream starts one marker block after logBootBase.
        const uint64_t stream = logBootBase + lumia950::kLogicalBlockBytes + from;
        const uint64_t slot = (stream / lumia950::kLogicalBlockBytes) % ringBlocks;
        uint64_t blocks = (head - from + lumia950::kLogicalBlockBytes - 1) / lumia950::kLogicalBlockBytes;
        if (blocks > kMaxTransferBlocks) blocks = kMaxTransferBlocks;
        if (blocks > ringBlocks - slot) blocks = ringBlocks - slot;
        const uint64_t length = blocks * lumia950::kLogicalBlockBytes;
        for (uint64_t i = 0; i < length; i++) {
            const uint64_t at = from + i;
            logBuffer[i] = at < head ? native_console_ring[at & (ringBytes - 1)] : 0;
        }
        if (!writeLogBlocks(logDataLba + slot, static_cast<uint32_t>(blocks), logBuffer)) return;
        from += length;
    }
    logFlushed = head;
    logFlushes++;
    writeLogHeader();
}

// The driver whose console log the panic path flushes.
static NativeSdhci *gConsoleLogDriver = nullptr;

// Called from debugger_collect_diagnostics (osfmk/kern/debug.c, overlay zzq) once the panic text is
// on the console: one last flush, without the lock, whatever the flush thread was doing. If the panic
// interrupted a transfer the card may refuse it; nothing else is lost by trying.
extern "C" void native_panic_flush(void)
{
    static bool flushed = false;
    NativeSdhci *driver = gConsoleLogDriver;
    if (flushed || !driver) return;
    flushed = true;
    driver->flushConsoleLog(true);
}

void NativeSdhci::consoleLogThread(void *self, wait_result_t)
{
    NativeSdhci *driver = static_cast<NativeSdhci *>(self);
    while (driver->logging) {
        driver->flushConsoleLog();
        IOSleep(1000);
    }
}

// Opens the log ring at the end of the writable window: only with writes enabled, boot-arg
// nativelog not 0, and only when the ring starts past the end of the HFS+ volume at the window's
// start (its volume header, block 2, gives the volume's length).
void NativeSdhci::startConsoleLog()
{
    uint32_t enable = 1;
    PE_parse_boot_argn("nativelog", &enable, sizeof(enable));
    if (!enable || !card.writable() || windowLast <= windowFirst) return;

    logDataLba = windowLast + 1 - lumia950::kConsoleLogBlocks;
    logHeaderLba = logDataLba - 1;
    alignas(8) uint8_t block[lumia950::kLogicalBlockBytes];
    IOLockLock(gate);
    const bool read = readBlocksPio(windowFirst + 2, 1, block);
    IOLockUnlock(gate);
    if (!read) return;
    const bool hfs = block[0] == 'H' && (block[1] == '+' || block[1] == 'X');
    const uint64_t volumeBlocks = static_cast<uint64_t>(be32(block + 44)) * be32(block + 40) /
                                  lumia950::kLogicalBlockBytes;
    if (!hfs || windowFirst + volumeBlocks > logHeaderLba) {
        IOLog("NativeSdhci: no console log: %s volume of %llu blocks reaches the ring at LBA %llu\n",
              hfs ? "HFS+" : "no HFS+", static_cast<unsigned long long>(volumeBlocks),
              static_cast<unsigned long long>(logHeaderLba));
        return;
    }

    IOLockLock(gate);
    const bool readHeader = readBlocksPio(logHeaderLba, 1, block);
    IOLockUnlock(gate);
    if (!readHeader) return;
    if (memcmp(block, kLogMagic, sizeof kLogMagic) == 0 && be32(block + 16) != 0) {
        const uint64_t previous = le64(block + 8);
        logBootBase = (previous + lumia950::kLogicalBlockBytes - 1) &
                      ~static_cast<uint64_t>(lumia950::kLogicalBlockBytes - 1);
        logBoot = (static_cast<uint32_t>(block[20]) | (static_cast<uint32_t>(block[21]) << 8) |
                   (static_cast<uint32_t>(block[22]) << 16) | (static_cast<uint32_t>(block[23]) << 24)) + 1;
    } else {
        logBootBase = 0;
        logBoot = 1;
    }

    logBuffer = static_cast<uint8_t *>(IOMallocAligned(kMaxTransferBlocks * lumia950::kLogicalBlockBytes, 64));
    if (!logBuffer) return;
    logging = true;

    memset(block, 0, sizeof block);
    snprintf(reinterpret_cast<char *>(block), sizeof block, "\n===== NATLOG boot %u =====\n", logBoot);
    const uint64_t markerSlot = (logBootBase / lumia950::kLogicalBlockBytes) % lumia950::kConsoleLogBlocks;
    if (!writeLogBlocks(logDataLba + markerSlot, 1, block)) return;
    writeLogHeader();

    thread_t thread = nullptr;
    if (kernel_thread_start(reinterpret_cast<thread_continue_t>(consoleLogThread), this, &thread) !=
        KERN_SUCCESS) {
        logging = false;
        IOLog("NativeSdhci: console log thread did not start\n");
        return;
    }
    thread_deallocate(thread);
    gConsoleLogDriver = this;
    IOLog("NativeSdhci: console log boot %u at LBA %llu (+%u ring blocks), stream offset %llu\n",
          logBoot, static_cast<unsigned long long>(logHeaderLba), lumia950::kConsoleLogBlocks,
          static_cast<unsigned long long>(logBootBase));
}

bool NativeSdhci::probeCard()
{
    const RegisterIo registers = {
        this, ioRead8, ioRead32, ioWrite8, ioWrite16, ioWrite32, ioDelay
    };
    if (!host.configure(registers)) {
        logFailure();
        return false;
    }
    if (!card.attach(host, lumia950::kRcaCandidates,
                     sizeof lumia950::kRcaCandidates / sizeof lumia950::kRcaCandidates[0])) {
        logFailure();
        IOLog("NativeSdhci: firmware card did not answer; identifying it from CMD0\n");
        if (!identifyCold()) return false;
    }
    relativeAddress = card.info().relativeAddress;
    capacityBytes = card.info().capacityBytes;
    blockCount = capacityBytes / lumia950::kLogicalBlockBytes;
    __atomic_store_n(&native_sdhci_rca, relativeAddress, __ATOMIC_RELAXED);
    native_sdhci_stage = 3;
    IOLog("NativeSdhci: RCA %u, CSD %u, MMC %u, EXT_CSD %u, user partition, 512-byte sectors, "
          "%u-bit bus at %u Hz\n",
          relativeAddress, card.info().csd.structure, card.info().csd.mmcVersion,
          card.info().extCsd.revision, card.info().busWidth, card.info().clockHz);

    alignas(8) uint8_t gpt[lumia950::kLogicalBlockBytes] = {};
    if (!readBlocksPio(lumia950::kGptHeaderLba, 1, gpt)) return false;
    if (memcmp(gpt, lumia950::kGptSignature, sizeof lumia950::kGptSignature) != 0) {
        IOLog("NativeSdhci: GPT signature missing; not publishing the firmware medium\n");
        return false;
    }
    native_sdhci_stage = 4;
    IOLog("NativeSdhci: GPT signature read successfully\n");
    findWritableWindow();
    setupDma();
    return card.ready();
}

bool NativeSdhci::start(IOService *provider)
{
    if (!IOBlockStorageDevice::start(provider)) return false;

    gate = IOLockAlloc();
    bounce = static_cast<uint8_t *>(IOMallocAligned(kMaxTransferBlocks * lumia950::kLogicalBlockBytes, 64));
    if (!gate || !bounce) {
        IOLog("NativeSdhci: no lock or bounce buffer\n");
        return false;
    }
    if (!mapWindows(provider)) return false;

    IOLog("NativeSdhci: hc at %p (virtual), core at %p\n",
          reinterpret_cast<void *>(const_cast<uint8_t *>(hc)),
          reinterpret_cast<void *>(const_cast<uint8_t *>(core)));

    logHost();
    logVendor();
    native_sdhci_stage = 0;

    if (!probeCard()) {
        IOLog("NativeSdhci: card probe failed; not publishing a medium\n");
        return false;
    }

    IOLockLock(gate);
    online = true;
    IOLockUnlock(gate);
    __atomic_store_n(&native_sdhci_ready, 1, __ATOMIC_RELAXED);

    IOLog("NativeSdhci: %llu blocks of %u bytes (%llu bytes), %s\n",
          static_cast<unsigned long long>(blockCount), lumia950::kLogicalBlockBytes,
          static_cast<unsigned long long>(capacityBytes),
          card.writable() ? "writable in the window above" : "read-only");
    registerService();
    startConsoleLog();
    return true;
}

void NativeSdhci::stop(IOService *provider)
{
    if (gate) {
        IOLockLock(gate);
        online = false;
        IOLockUnlock(gate);
    }
    card.detach();
    host.stop();
    __atomic_store_n(&native_sdhci_ready, 0, __ATOMIC_RELAXED);
    IOBlockStorageDevice::stop(provider);
}

void NativeSdhci::free()
{
    card.detach();
    host.stop();
    if (hcMap) { hcMap->release(); hcMap = nullptr; hc = nullptr; }
    if (coreMap) { coreMap->release(); coreMap = nullptr; core = nullptr; }
    if (gccMap) { gccMap->release(); gccMap = nullptr; gcc = nullptr; }
    if (gate) { IOLockFree(gate); gate = nullptr; }
    if (logBuffer) {
        IOFreeAligned(logBuffer, kMaxTransferBlocks * lumia950::kLogicalBlockBytes);
        logBuffer = nullptr;
    }
    if (bounce) {
        IOFreeAligned(bounce, kMaxTransferBlocks * lumia950::kLogicalBlockBytes);
        bounce = nullptr;
    }
    OSSafeReleaseNULL(dmaBuffer);
    OSSafeReleaseNULL(dmaTable);
    IOBlockStorageDevice::free();
}

// ---------------------------------------------------------------------------
// IOBlockStorageDevice
// ---------------------------------------------------------------------------

IOReturn NativeSdhci::reportBlockSize(UInt64 *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = lumia950::kLogicalBlockBytes;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportEjectability(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportMaxValidBlock(UInt64 *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = blockCount ? blockCount - 1 : 0;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportMediaState(bool *present, bool *changed)
{
    if (!present) return kIOReturnBadArgument;
    *present = online;
    if (changed) *changed = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportRemovability(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportWriteProtection(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    // Writable only when a GPT window is open (partition kWritablePartition and boot-arg
    // sdhcirw=1); writes outside it are refused per request in doAsyncReadWrite.
    *value = !card.writable();
    return kIOReturnSuccess;
}

// A request refused before it starts surfaces only as IOBlockStorageDriver's "request failed to start";
// say why, for the first few of each kind.
static void refused(const char *why, UInt64 block, UInt64 count)
{
    static volatile uint32_t logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 16) {
        IOLog("NativeSdhci: request refused (%s): LBA %llu +%llu\n", why, static_cast<unsigned long long>(block),
              static_cast<unsigned long long>(count));
    }
}

IOReturn NativeSdhci::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                       IOStorageAttributes *, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count) {
        refused("no buffer, completion or count", block, count);
        return kIOReturnBadArgument;
    }
    if (block >= blockCount || count > blockCount - block) {
        refused("beyond the medium", block, count);
        return kIOReturnBadArgument;
    }
    const IODirection direction = buffer->getDirection();
    if (direction != kIODirectionIn && direction != kIODirectionOut) {
        refused("direction", block, count);
        return kIOReturnBadArgument;
    }
    if (count > ~static_cast<uint64_t>(0) / lumia950::kLogicalBlockBytes) {
        refused("size overflow", block, count);
        return kIOReturnBadArgument;
    }
    const uint64_t bytes = count * lumia950::kLogicalBlockBytes;
    if (buffer->getLength() < bytes) {
        refused("buffer shorter than the request", block, count);
        return kIOReturnBadArgument;
    }

    IOLockLock(gate);
    if (!online) {
        IOLockUnlock(gate);
        refused("medium offline after an I/O failure", block, count);
        return kIOReturnNotReady;
    }

    uint64_t done = 0;
    IOReturn status = kIOReturnSuccess;
    while (done < bytes) {
        const uint64_t lba = block + done / lumia950::kLogicalBlockBytes;
        const uint64_t left = (bytes - done) / lumia950::kLogicalBlockBytes;
        const uint32_t most = direction == kIODirectionIn && dma ? kMaxDmaBlocks : kMaxTransferBlocks;
        const uint32_t chunk = left > most ? most : static_cast<uint32_t>(left);
        const uint64_t length = static_cast<uint64_t>(chunk) * lumia950::kLogicalBlockBytes;
        if (direction == kIODirectionIn) {
            const uint8_t *data = bounce;
            if (dma) {
                if (readBlocksDma(lba, chunk)) {
                    data = static_cast<const uint8_t *>(dmaBuffer->getBytesNoCopy());
                } else {
                    // Back to PIO for good: reset the lines and the card, then this stretch again in
                    // PIO-sized pieces (the bounce buffer holds kMaxTransferBlocks).
                    IOLog("NativeSdhci: ADMA2 read failed at LBA %llu +%u (host error %u phase %u, adma error "
                          "0x%02x); PIO from now on\n", static_cast<unsigned long long>(lba), chunk,
                          static_cast<unsigned>(host.result().error), static_cast<unsigned>(host.result().phase),
                          readHc8(kRegAdmaErrorStatus));
                    dma = false;
                    card.recover();
                    continue;
                }
            }
            if (data == bounce && !readBlocksPio(lba, chunk, bounce)) {
                status = kIOReturnIOError;
                break;
            }
            if (buffer->writeBytes(done, data, length) != length) {
                status = kIOReturnIOError;
                break;
            }
        } else {
            if (buffer->readBytes(done, bounce, length) != length) {
                status = kIOReturnIOError;
                break;
            }
            if (!card.writeBlocks(lba, chunk, bounce)) {
                if (card.error() == WarmCardError::WriteRefused) {
                    if (!writeRefused) {
                        writeRefused = true;
                        IOLog("NativeSdhci: refusing a write at LBA %llu (+%u): outside the '%s' window "
                              "or writes are off\n", static_cast<unsigned long long>(lba), chunk,
                              lumia950::kWritablePartition);
                    }
                    status = kIOReturnNotPermitted;
                } else {
                    online = false;
                    __atomic_store_n(&native_sdhci_ready, 0, __ATOMIC_RELAXED);
                    logFailure();
                    status = kIOReturnIOError;
                }
                break;
            }
            __atomic_fetch_add(&native_sdhci_writes, 1, __ATOMIC_RELAXED);
        }
        done += length;
    }

    __atomic_fetch_add(&native_sdhci_bytes, done, __ATOMIC_RELAXED);
    if (done && direction == kIODirectionIn) __atomic_fetch_add(&native_sdhci_reads, 1, __ATOMIC_RELAXED);
    IOLockUnlock(gate);

    completion->action(completion->target, completion->parameter, status, done);
    return kIOReturnSuccess;
}
