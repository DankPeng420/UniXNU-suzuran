#ifndef NATIVE_SDHCI_POLLING_HOST_H
#define NATIVE_SDHCI_POLLING_HOST_H

#include "SdhciCore.h"

namespace sdhci {

struct RegisterIo {
    void *context;
    u8 (*read8)(void *, u32);
    u32 (*read32)(void *, u32);
    void (*write8)(void *, u32, u8);
    void (*write16)(void *, u32, u16);
    void (*write32)(void *, u32, u32);
    void (*delayUs)(void *, u32);
};

struct PollLimits {
    u32 commandPolls = 10000;
    u32 transferPolls = 50000;   // per 512-byte block
    u32 resetPolls = 10000;
    u32 delayUs = 10;
    u32 cardPolls = 100;
};

// Most blocks one data command moves (the block count register is 16 bits; this bounds a
// single PIO burst and the caller's bounce buffer).
constexpr u32 kMaxTransferBlocks = 128;
// Most blocks one ADMA2 read moves (512 KiB): fewer commands, each paying the card's access time once
// (boot 19: 64 KiB commands read at ~27 MB/s on a 52 MB/s bus).
constexpr u32 kMaxDmaBlocks = 1024;

// R3 is the OCR answer to CMD1: 48 bits with neither CRC nor index.
enum class ResponseType : u8 { None, R1, R1Busy, R2, R3 };
enum class HostError : u8 {
    None, InvalidArgument, NotReady, ResetTimeout, InhibitTimeout,
    CommandTimeout, DataTimeout, Controller, CardStatus, CardState
};
enum class HostPhase : u8 { Idle, Reset, Inhibit, Command, Buffer, Transfer, Card };

struct HostResult {
    HostError error = HostError::None;
    HostPhase phase = HostPhase::Idle;
    u32 interruptStatus = 0;
    u32 cardStatus = 0;
    u32 bytesTransferred = 0;
    u8 command = 0;
};

// One issued command and how it ended, for the bring-up log.
struct CommandTrace {
    u8 index;
    u32 argument;
    u32 status;
    u32 response;
};

class PollingHost {
public:
    bool configure(const RegisterIo &registers, const PollLimits &pollLimits = {});
    bool start();
    void stop();
    bool command(u8 index, u32 argument, ResponseType type, u32 response[4]);
    // Programmed-I/O data commands. Reads: CMD8 (EXT_CSD) and CMD17 one block, CMD18 two or more.
    // Writes: CMD24 one block, CMD25 two or more. Multi-block commands end with Auto CMD12.
    // A failed read leaves the destination partly written; nothing reports it as data.
    bool readBlocks(u8 index, u32 argument, u8 *destination, u32 blocks);
    // The same reads by ADMA2: the host moves the data itself to where the 32-bit descriptor table at
    // tablePhys points (table and buffers below 4 GiB, the table already cleaned to memory); only the
    // transfer completion is polled.
    bool readBlocksDma(u8 index, u32 argument, u32 tablePhys, u32 blocks);
    bool writeBlocks(u8 index, u32 argument, const u8 *source, u32 blocks);
    bool waitCardReady(u16 relativeAddress, u32 state, u32 polls = 0);
    void pause(u32 microseconds) { if (io.delayUs) io.delayUs(io.context, microseconds); }
    // Delay after every interrupt-status clear. The QSDCC host synchronises the clear
    // into the card clock domain, so at 400 kHz it lands late (sdhci-msm's
    // SDHCI_QUIRK2_SLOW_INT_CLR waits 40 us there).
    void setClearSettle(u32 microseconds) { settle = microseconds; }
    // The `back`-th most recent command (0 is the last), or nullptr.
    const CommandTrace *recent(u32 back) const
    {
        return back < kTraceDepth && back < issued ? &history[(issued - 1 - back) % kTraceDepth] : nullptr;
    }
    bool ready() const { return running; }
    const HostResult &result() const { return last; }

private:
    struct DataPhase {
        u8 *in;
        const u8 *out;
        u32 blocks;
        u32 dmaTable;   // nonzero: an ADMA2 read through this descriptor table, in and out unused
    };
    bool issue(u8 index, u32 argument, ResponseType type, const DataPhase *data, u32 response[4]);
    bool moveData(const DataPhase &data);
    bool waitStatus(u32 wanted, u32 &remaining, HostPhase phase, HostError timeout);
    bool fail(HostError error, HostPhase phase, u32 status = 0);
    void acknowledge(u32 status);
    bool validIo() const;

    static constexpr u32 kTraceDepth = 8;
    RegisterIo io = {};
    PollLimits limits = {};
    HostResult last = {};
    CommandTrace history[kTraceDepth] = {};
    u32 issued = 0;
    u32 lastStatus = 0;
    u32 settle = 0;
    bool running = false;
};

enum class WarmCardError : u8 {
    None, InvalidArgument, Host, NoRelativeAddress, UnexpectedState,
    InvalidCsd, InvalidExtCsd, UnsupportedCard, WrongPartition, CommandQueue,
    BusClock, OperatingCondition, BusMode, WriteRefused
};

// The board's card clock and bus. A cold identification runs at 400 kHz on one data line and the
// transfer state at the rate the board picks. setClock also selects the host's high-speed timing for
// rates above 26 MHz; setBusWidth takes 1 or 8.
struct BusControl {
    void *context;
    bool (*setClock)(void *, u32 hz);
    bool (*setBusWidth)(void *, u32 bits);
};

struct WarmCardInfo {
    u16 relativeAddress = 0;
    u32 ocr = 0;
    u32 cid[4] = {};
    bool cold = false;
    u32 busWidth = 1;
    u32 clockHz = 0;
    CsdInfo csd = {};
    ExtCsdInfo extCsd = {};
    u64 capacityBytes = 0;
};

class WarmCard {
public:
    bool attach(PollingHost &controller, const u16 *candidates, u32 count);
    // Identify the card from CMD0, for when the firmware's selected card does not
    // answer: assigns relativeAddress and leaves the card selected at transferHz.
    bool initialize(PollingHost &controller, u16 relativeAddress, const BusControl &bus, u32 transferHz);
    // After a cold initialize: 8-bit bus, then high-speed timing at highSpeedHz when EXT_CSD
    // offers it. Each step is proved by reading EXT_CSD back through the new bus mode. On false
    // the card is in an unknown bus mode and must be initialized again.
    bool upgrade(const BusControl &bus, u32 highSpeedHz);
    bool readBlocks(u64 lba, u32 count, u8 *destination);
    bool readBlock(u64 lba, u8 *destination) { return readBlocks(lba, 1, destination); }
    // ADMA2 read of count blocks through the descriptor table at tablePhys (see PollingHost::readBlocksDma).
    bool readBlocksDma(u64 lba, u32 count, u32 tablePhys);
    // Writes are refused unless every block lies in the window allowWrites opened.
    void allowWrites(u64 firstLba, u64 lastLba);
    bool writeBlocks(u64 lba, u32 count, const u8 *source);
    bool writable() const { return writeLast >= writeFirst && writeLast; }
    // For a transfer that was cut off (a panic in the middle of one): reset the host's command and
    // data lines, stop whatever the card was transferring and wait for it to return to transfer state.
    bool recover();
    void detach();
    bool ready() const { return attached && host && host->ready(); }
    const WarmCardInfo &info() const { return card; }
    WarmCardError error() const { return lastError; }

private:
    bool fail(WarmCardError error);
    bool finish(bool selected);
    bool switchExtCsd(u8 index, u8 value);
    bool readExtCsd();
    PollingHost *host = nullptr;
    WarmCardInfo card = {};
    WarmCardError lastError = WarmCardError::None;
    u64 writeFirst = 1;
    u64 writeLast = 0;
    bool attached = false;
};

const char *hostErrorName(HostError error);

}

#endif
