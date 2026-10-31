// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NATIVE_SDHCI_H
#define NATIVE_SDHCI_H

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include "PollingHost.h"

// IOMemoryMap has no header of its own in the kernel tree: IOService.h declares the
// name for mapDeviceMemoryWithIndex(), and NativeSdhci.cpp includes the descriptor
// header for the complete class.

class NativeSdhci : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(NativeSdhci)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    // IOBlockStorageDevice
    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
    UInt32   doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    char    *getVendorString() override { return const_cast<char *>("Native"); }
    char    *getProductString() override { return const_cast<char *>("Lumia 950 eMMC"); }
    char    *getRevisionString() override { return const_cast<char *>("0.1"); }
    char    *getAdditionalDeviceInfoString() override { return const_cast<char *>("writes only inside GPT 'ios', with sdhcirw=1"); }
    IOReturn reportBlockSize(UInt64 *value) override;
    IOReturn reportEjectability(bool *value) override;
    IOReturn reportMaxValidBlock(UInt64 *value) override;
    IOReturn reportMediaState(bool *present, bool *changed) override;
    IOReturn reportRemovability(bool *value) override;
    IOReturn reportWriteProtection(bool *value) override;
    IOReturn doSynchronizeCache() override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                              IOStorageAttributes *attributes, IOStorageCompletion *completion) override;

private:
    // Register access. SDHCI mixes 8-, 16- and 32-bit registers, so the width is
    // explicit rather than derived from a 32-bit read.
    uint8_t  readHc8(uint32_t offset) const;
    uint16_t readHc16(uint32_t offset) const;
    uint32_t readHc32(uint32_t offset) const;
    void     writeHc8(uint32_t offset, uint8_t value);
    void     writeHc16(uint32_t offset, uint16_t value);
    void     writeHc32(uint32_t offset, uint32_t value);
    uint32_t readCore32(uint32_t offset) const;
    void     writeCore32(uint32_t offset, uint32_t value);
    uint32_t readGcc32(uint32_t offset) const;
    void     writeGcc32(uint32_t offset, uint32_t value);

    bool mapWindows(IOService *provider);
    void logHost();
    void logVendor();
    bool setBusClock(uint32_t hz);
    bool setBusWidth(uint32_t bits);
    static bool busClock(void *context, sdhci::u32 hz);
    static bool busWidth(void *context, sdhci::u32 bits);
    bool identifyCold();
    void findWritableWindow();
    bool readBlocksPio(uint64_t lba, uint32_t count, void *bytes);
    void setupDma();
    bool readBlocksDma(uint64_t lba, uint32_t count);
    void startConsoleLog();
    bool writeLogBlocks(uint64_t lba, uint32_t count, const uint8_t *bytes);
    void writeLogHeader();

public:
    // panicking: called from the panic path with everything else stopped; takes no lock and logs
    // nothing (native_panic_flush).
    void flushConsoleLog(bool panicking = false);

private:
    static void consoleLogThread(void *self, wait_result_t);
    bool probeCard();
    void logFailure();
    static sdhci::u8 ioRead8(void *context, sdhci::u32 offset);
    static sdhci::u32 ioRead32(void *context, sdhci::u32 offset);
    static void ioWrite8(void *context, sdhci::u32 offset, sdhci::u8 value);
    static void ioWrite16(void *context, sdhci::u32 offset, sdhci::u16 value);
    static void ioWrite32(void *context, sdhci::u32 offset, sdhci::u32 value);
    static void ioDelay(void *context, sdhci::u32 microseconds);

    IOLock            *gate = nullptr;
    volatile uint8_t  *hc = nullptr;
    volatile uint8_t  *core = nullptr;
    IOMemoryMap       *hcMap = nullptr;
    IOMemoryMap       *coreMap = nullptr;
    IOMemoryMap       *gccMap = nullptr;
    volatile uint8_t  *gcc = nullptr;
    uint8_t           *bounce = nullptr;   // kMaxTransferBlocks * 512 bytes
    // ADMA2 reads (setupDma): a physically contiguous kMaxDmaBlocks * 512-byte buffer and a page
    // for the descriptor table, both below 4 GiB; dma is cleared for good after any DMA failure.
    IOBufferMemoryDescriptor *dmaBuffer = nullptr;
    IOBufferMemoryDescriptor *dmaTable = nullptr;
    uint32_t           dmaBufferPhys = 0;
    uint32_t           dmaTablePhys = 0;
    bool               dma = false;

    uint32_t baseClockHz = 0;
    uint16_t hostVersion = 0;
    uint16_t relativeAddress = 0;
    uint64_t capacityBytes = 0;
    uint64_t blockCount = 0;
    bool     online = false;
    bool     writeRefused = false;
    uint64_t windowFirst = 0;
    uint64_t windowLast = 0;

    // Console log on the medium (see startConsoleLog). Stream offsets count bytes from the
    // start of the log ever written; this boot's console ring byte i is at logBootBase + i.
    uint64_t logHeaderLba = 0;
    uint64_t logDataLba = 0;
    uint64_t logBootBase = 0;
    uint64_t logFlushed = 0;     // console ring bytes already on the medium
    uint64_t logLost = 0;
    uint64_t logFlushes = 0;
    uint32_t logBoot = 0;
    bool     logging = false;
    bool     logPanicking = false;
    uint8_t *logBuffer = nullptr;   // kMaxTransferBlocks * 512 bytes
    sdhci::PollingHost host;
    sdhci::WarmCard card;
};

#endif // NATIVE_SDHCI_H
