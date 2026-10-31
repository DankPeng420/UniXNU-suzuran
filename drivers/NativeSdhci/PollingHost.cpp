#include "PollingHost.h"
#include "SdhciRegs.h"

namespace sdhci {
namespace {
constexpr u32 kErrorStatus = 0xffff8000u;
constexpr u32 kPollingStatus = kIntCommandComplete | kIntTransferComplete |
                               kIntBufferReadReady | kIntBufferWriteReady | kIntError | 0x03ff0000u;
constexpr u32 kCardErrors = 0xffff0000u | (1u << 7);
constexpr u32 kReadyForData = 1u << 8;
constexpr u32 kStateStandby = 3;
constexpr u32 kStateTransfer = 4;
// CMD1 argument: sector addressing, 2.7-3.6 V and 1.70-1.95 V.
constexpr u32 kOcrRequest = 0x40ff8080u;
constexpr u32 kOcrReady = 1u << 31;
constexpr u32 kOcrSectorMode = 1u << 30;
constexpr u32 kIdentificationHz = 400000;
constexpr u32 kOperatingConditionPolls = 1000;
// CMD13 polls (1 ms apart) after a write or a CMD6 switch: programming may take a while.
constexpr u32 kProgramPolls = 2000;
constexpr u8 kExtCsdBusWidth = 183;
constexpr u8 kExtCsdHsTiming = 185;

u32 cardState(u32 response)
{
    return (response >> 9) & 15;
}
}

bool PollingHost::validIo() const
{
    return io.read8 && io.read32 && io.write8 && io.write16 && io.write32 && io.delayUs;
}

bool PollingHost::configure(const RegisterIo &registers, const PollLimits &pollLimits)
{
    if (running) return false;
    io = registers;
    limits = pollLimits;
    last = {};
    if (!validIo() || !limits.commandPolls || limits.commandPolls > 1000000 ||
        !limits.transferPolls || limits.transferPolls > 1000000 ||
        !limits.resetPolls || limits.resetPolls > 1000000 ||
        !limits.delayUs || limits.delayUs > 1000 ||
        !limits.cardPolls || limits.cardPolls > 1000) {
        io = {};
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }
    return true;
}

bool PollingHost::fail(HostError error, HostPhase phase, u32 status)
{
    last.error = error;
    last.phase = phase;
    last.interruptStatus = status;
    running = false;
    return false;
}

void PollingHost::acknowledge(u32 status)
{
    io.write32(io.context, kRegInterruptStatus, status & (kPollingStatus | kErrorStatus));
    if (settle) io.delayUs(io.context, settle);
}

bool PollingHost::start()
{
    running = false;
    last = {};
    if (!validIo()) return fail(HostError::InvalidArgument, HostPhase::Reset);
    io.write32(io.context, kRegInterruptSignal, 0);
    io.write8(io.context, kRegSoftwareReset, kResetCommand | kResetData);
    bool reset = false;
    for (u32 poll = 0; poll < limits.resetPolls; ++poll) {
        if (!(io.read8(io.context, kRegSoftwareReset) & (kResetCommand | kResetData))) {
            reset = true;
            break;
        }
        io.delayUs(io.context, limits.delayUs);
    }
    if (!reset) return fail(HostError::ResetTimeout, HostPhase::Reset);
    io.write8(io.context, kRegTimeoutControl, 0x0e);
    io.write32(io.context, kRegInterruptEnable, kPollingStatus);
    acknowledge(kPollingStatus);
    running = true;
    return true;
}

void PollingHost::stop()
{
    if (validIo()) io.write32(io.context, kRegInterruptSignal, 0);
    running = false;
}

bool PollingHost::waitStatus(u32 wanted, u32 &remaining, HostPhase phase, HostError timeout)
{
    u32 status = 0;
    while (remaining) {
        --remaining;
        status = io.read32(io.context, kRegInterruptStatus);
        lastStatus = status;
        if (status & kErrorStatus) return fail(HostError::Controller, phase, status);
        if ((status & wanted) == wanted) return true;
        io.delayUs(io.context, limits.delayUs);
    }
    return fail(timeout, phase, status);
}

bool PollingHost::issue(u8 index, u32 argument, ResponseType type, const DataPhase *data, u32 response[4])
{
    last = {};
    last.command = index;
    if (!running) return fail(HostError::NotReady, HostPhase::Idle);
    if (index > 63 || static_cast<u8>(type) > static_cast<u8>(ResponseType::R3) ||
        (type != ResponseType::None && !response)) {
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }
    if (data && (type != ResponseType::R1 || !data->blocks ||
                 data->blocks > (data->dmaTable ? kMaxDmaBlocks : kMaxTransferBlocks) ||
                 (data->dmaTable ? (data->in || data->out) : !data->in == !data->out))) {
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }

    const u32 inhibit = kPresentCmdInhibit |
                        ((data || type == ResponseType::R1Busy) ? static_cast<u32>(kPresentDataInhibit) : 0u);
    bool available = false;
    for (u32 poll = 0; poll < limits.commandPolls; ++poll) {
        if (!(io.read32(io.context, kRegPresentState) & inhibit)) {
            available = true;
            break;
        }
        io.delayUs(io.context, limits.delayUs);
    }
    if (!available) return fail(HostError::InhibitTimeout, HostPhase::Inhibit);

    acknowledge(kPollingStatus | kErrorStatus);
    u16 mode = 0;
    if (data) {
        io.write16(io.context, kRegBlockSize, 512);
        io.write16(io.context, kRegBlockCount, static_cast<u16>(data->blocks));
        mode = kModeBlockCountEnable | ((data->in || data->dmaTable) ? kModeDirectionRead : 0);
        if (data->blocks > 1) mode |= kModeMultiBlock | kModeAutoCmd12;
        if (data->dmaTable) {
            io.write32(io.context, kRegAdmaAddress, data->dmaTable);
            io.write32(io.context, kRegAdmaAddress + 4, 0);
            io.write8(io.context, kRegHostControl1, static_cast<u8>((io.read8(io.context, kRegHostControl1) &
                                                                     ~kHostDmaSelectMask) | kHostDmaSelectAdma2));
            mode |= kModeDmaEnable;
        }
    }
    io.write16(io.context, kRegTransferMode, mode);
    u16 flags = data ? kCmdDataPresent : 0;
    switch (type) {
    case ResponseType::None: break;
    case ResponseType::R1: flags |= kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck; break;
    case ResponseType::R1Busy: flags |= kCmdResponse48Busy | kCmdCrcCheck | kCmdIndexCheck; break;
    case ResponseType::R2: flags |= kCmdResponse136 | kCmdCrcCheck; break;
    case ResponseType::R3: flags |= kCmdResponse48; break;
    }
    io.write32(io.context, kRegArgument, argument);
    io.write16(io.context, kRegCommand, static_cast<u16>((index << 8) | flags));
    CommandTrace &trace = history[issued++ % kTraceDepth];
    trace = {index, argument, 0, 0};
    lastStatus = 0;
    u32 remaining = limits.commandPolls;
    const bool completed = waitStatus(kIntCommandComplete, remaining, HostPhase::Command,
                                      HostError::CommandTimeout);
    trace.status = lastStatus;
    if (!completed) return false;
    if (type != ResponseType::None) trace.response = io.read32(io.context, kRegResponse);
    if (response) {
        for (u32 word = 0; word < 4; ++word) response[word] = 0;
        if (type == ResponseType::R2) {
            u32 registers[4];
            for (u32 word = 0; word < 4; ++word) {
                registers[word] = io.read32(io.context, kRegResponse + word * 4);
            }
            normalizeResponse136(registers, response);
        } else if (type != ResponseType::None) {
            response[0] = io.read32(io.context, kRegResponse);
        }
    }
    acknowledge(kIntCommandComplete);
    if (type == ResponseType::R1 || type == ResponseType::R1Busy) {
        last.cardStatus = response[0];
        if (response[0] & kCardErrors) return fail(HostError::CardStatus, HostPhase::Card);
    }
    if (type == ResponseType::R1Busy) {
        remaining = limits.transferPolls;
        if (!waitStatus(kIntTransferComplete, remaining, HostPhase::Transfer, HostError::DataTimeout)) {
            return false;
        }
        acknowledge(kIntTransferComplete);
    }
    return !data || moveData(*data);
}

// One 512-byte block per buffer-ready event, then the transfer completion. Errors are checked
// between blocks rather than between words: a status read per FIFO word would double the MMIO
// cost of every block, and a damaged block is never reported as transferred either way.
bool PollingHost::moveData(const DataPhase &data)
{
    if (data.dmaTable) {
        // The host walks the descriptor table; an ADMA error (or any other) ends the wait.
        u32 remaining = limits.transferPolls;
        if (!waitStatus(kIntTransferComplete, remaining, HostPhase::Transfer, HostError::DataTimeout)) return false;
        acknowledge(kIntTransferComplete);
        last.bytesTransferred = data.blocks * 512;
        return true;
    }
    const bool write = data.out != nullptr;
    const u32 readyBit = write ? kIntBufferWriteReady : kIntBufferReadReady;
    const u32 presentBit = write ? kPresentBufferWrite : kPresentBufferRead;
    for (u32 block = 0; block < data.blocks; ++block) {
        u32 remaining = limits.transferPolls;
        if (!waitStatus(readyBit, remaining, HostPhase::Buffer, HostError::DataTimeout)) return false;
        acknowledge(readyBit);
        bool available = false;
        while (remaining) {
            --remaining;
            const u32 status = io.read32(io.context, kRegInterruptStatus);
            if (status & kErrorStatus) return fail(HostError::Controller, HostPhase::Buffer, status);
            if (io.read32(io.context, kRegPresentState) & presentBit) {
                available = true;
                break;
            }
            io.delayUs(io.context, limits.delayUs);
        }
        if (!available) return fail(HostError::DataTimeout, HostPhase::Buffer);
        if (write) {
            const u8 *bytes = data.out + block * 512;
            for (u32 word = 0; word < 128; ++word) {
                const u8 *at = bytes + word * 4;
                io.write32(io.context, kRegBufferPort,
                           static_cast<u32>(at[0]) | (static_cast<u32>(at[1]) << 8) |
                           (static_cast<u32>(at[2]) << 16) | (static_cast<u32>(at[3]) << 24));
            }
        } else {
            u8 *bytes = data.in + block * 512;
            for (u32 word = 0; word < 128; ++word) {
                const u32 value = io.read32(io.context, kRegBufferPort);
                for (u32 byte = 0; byte < 4; ++byte) bytes[word * 4 + byte] = static_cast<u8>(value >> (byte * 8));
            }
        }
        const u32 status = io.read32(io.context, kRegInterruptStatus);
        if (status & kErrorStatus) return fail(HostError::Controller, HostPhase::Buffer, status);
    }
    // A write completes after the card releases its busy signal, which includes programming.
    u32 remaining = write ? limits.transferPolls * 4 : limits.transferPolls;
    if (!waitStatus(kIntTransferComplete, remaining, HostPhase::Transfer, HostError::DataTimeout)) {
        return false;
    }
    acknowledge(kIntTransferComplete);
    last.bytesTransferred = data.blocks * 512;
    return true;
}

bool PollingHost::command(u8 index, u32 argument, ResponseType type, u32 response[4])
{
    return issue(index, argument, type, nullptr, response);
}

bool PollingHost::readBlocks(u8 index, u32 argument, u8 *destination, u32 blocks)
{
    const bool single = index == kCmd8SendExtCsd || index == kCmd17ReadSingleBlock;
    if (!destination || !(single ? blocks == 1 : index == kCmd18ReadMultiple && blocks >= 2)) {
        last = {};
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }
    const DataPhase data = {destination, nullptr, blocks, 0};
    u32 response[4];
    return issue(index, argument, ResponseType::R1, &data, response);
}

bool PollingHost::readBlocksDma(u8 index, u32 argument, u32 tablePhys, u32 blocks)
{
    if (!tablePhys || (tablePhys & 3) || !(index == kCmd17ReadSingleBlock ? blocks == 1
                                                                          : index == kCmd18ReadMultiple && blocks >= 2)) {
        last = {};
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }
    const DataPhase data = {nullptr, nullptr, blocks, tablePhys};
    u32 response[4];
    return issue(index, argument, ResponseType::R1, &data, response);
}

bool PollingHost::writeBlocks(u8 index, u32 argument, const u8 *source, u32 blocks)
{
    if (!source || !(index == kCmd24WriteSingleBlock ? blocks == 1 : index == kCmd25WriteMultiple && blocks >= 2)) {
        last = {};
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }
    const DataPhase data = {nullptr, source, blocks, 0};
    u32 response[4];
    return issue(index, argument, ResponseType::R1, &data, response);
}

bool PollingHost::waitCardReady(u16 relativeAddress, u32 state, u32 polls)
{
    if (!relativeAddress || (state != kStateStandby && state != kStateTransfer)) {
        last = {};
        return fail(HostError::InvalidArgument, HostPhase::Card);
    }
    if (!polls) polls = limits.cardPolls;
    for (u32 poll = 0; poll < polls; ++poll) {
        u32 response[4];
        if (!command(kCmd13SendStatus, static_cast<u32>(relativeAddress) << 16,
                     ResponseType::R1, response)) return false;
        if ((response[0] & kReadyForData) && cardState(response[0]) == state) return true;
        io.delayUs(io.context, 1000);
    }
    return fail(HostError::CardState, HostPhase::Card);
}

bool WarmCard::fail(WarmCardError error)
{
    lastError = error;
    attached = false;
    return false;
}

void WarmCard::detach()
{
    attached = false;
    host = nullptr;
}

bool WarmCard::attach(PollingHost &controller, const u16 *candidates, u32 count)
{
    detach();
    card = {};
    lastError = WarmCardError::None;
    host = &controller;
    if (!candidates || !count || count > 16) return fail(WarmCardError::InvalidArgument);
    u32 response[4];
    bool selected = false;
    for (u32 candidate = 0; candidate < count; ++candidate) {
        if (!candidates[candidate]) continue;
        if (!host->start()) return fail(WarmCardError::Host);
        if (!host->command(kCmd13SendStatus, static_cast<u32>(candidates[candidate]) << 16,
                           ResponseType::R1, response)) continue;
        const u32 state = cardState(response[0]);
        if (!(response[0] & kReadyForData) || (state != kStateStandby && state != kStateTransfer)) {
            return fail(WarmCardError::UnexpectedState);
        }
        card.relativeAddress = candidates[candidate];
        selected = state == kStateTransfer;
        break;
    }
    if (!card.relativeAddress) return fail(WarmCardError::NoRelativeAddress);
    return finish(selected);
}

bool WarmCard::initialize(PollingHost &controller, u16 relativeAddress, const BusControl &bus, u32 transferHz)
{
    detach();
    card = {};
    lastError = WarmCardError::None;
    host = &controller;
    if (!relativeAddress || !bus.setClock || !bus.setBusWidth || !transferHz || transferHz > 26000000) {
        return fail(WarmCardError::InvalidArgument);
    }
    card.cold = true;
    if (!bus.setBusWidth(bus.context, 1) || !bus.setClock(bus.context, kIdentificationHz)) {
        return fail(WarmCardError::BusClock);
    }
    host->setClearSettle(40);
    if (!host->start() || !host->command(kCmd0GoIdleState, 0, ResponseType::None, nullptr)) {
        return fail(WarmCardError::Host);
    }
    host->pause(1000);
    u32 response[4];
    for (u32 poll = 0; poll < kOperatingConditionPolls; ++poll) {
        if (!host->command(kCmd1SendOpCond, kOcrRequest, ResponseType::R3, response)) {
            return fail(WarmCardError::Host);
        }
        card.ocr = response[0];
        if (card.ocr & kOcrReady) break;
        host->pause(1000);
    }
    if (!(card.ocr & kOcrReady)) return fail(WarmCardError::OperatingCondition);
    if (!(card.ocr & kOcrSectorMode)) return fail(WarmCardError::UnsupportedCard);
    if (!host->command(kCmd2AllSendCid, 0, ResponseType::R2, response)) return fail(WarmCardError::Host);
    for (u32 word = 0; word < 4; ++word) card.cid[word] = response[word];
    // CMD3 once completed nothing on hardware (no response, no timeout status). Reset the
    // command line and send it again; if the card already took the address it no longer
    // answers CMD3 in standby, so CMD13 settles which case it was.
    const u32 address = static_cast<u32>(relativeAddress) << 16;
    bool assigned = host->command(kCmd3SetRelativeAddr, address, ResponseType::R1, response);
    if (!assigned && host->start()) {
        assigned = host->command(kCmd3SetRelativeAddr, address, ResponseType::R1, response);
        if (!assigned && host->start()) {
            assigned = host->command(kCmd13SendStatus, address, ResponseType::R1, response) &&
                       cardState(response[0]) == kStateStandby;
        }
    }
    if (!assigned) return fail(WarmCardError::Host);
    card.relativeAddress = relativeAddress;
    if (!bus.setClock(bus.context, transferHz)) return fail(WarmCardError::BusClock);
    host->setClearSettle(0);
    card.clockHz = transferHz;
    return finish(false);
}

bool WarmCard::finish(bool selected)
{
    u32 response[4];
    if (selected && !host->command(kCmd7SelectCard, 0, ResponseType::None, nullptr)) {
        return fail(WarmCardError::Host);
    }
    if (!host->waitCardReady(card.relativeAddress, kStateStandby) ||
        !host->command(kCmd9SendCsd, static_cast<u32>(card.relativeAddress) << 16,
                       ResponseType::R2, response)) return fail(WarmCardError::Host);
    const bool validCsd = decodeCsd(response, &card.csd);
    if (!host->command(kCmd7SelectCard, static_cast<u32>(card.relativeAddress) << 16,
                       ResponseType::R1, response) ||
        !host->waitCardReady(card.relativeAddress, kStateTransfer)) return fail(WarmCardError::Host);
    if (!validCsd) return fail(WarmCardError::InvalidCsd);
    if (card.csd.mmcVersion < 4) return fail(WarmCardError::UnsupportedCard);
    if (!readExtCsd()) return false;
    if (card.extCsd.partitionAccess) return fail(WarmCardError::WrongPartition);
    if (card.extCsd.commandQueueEnabled) return fail(WarmCardError::CommandQueue);
    if (card.extCsd.revision < 2 || card.extCsd.sectorBytes != 512 ||
        card.extCsd.capacityBytes <= (2ULL << 30)) return fail(WarmCardError::UnsupportedCard);
    if (!host->waitCardReady(card.relativeAddress, kStateTransfer)) return fail(WarmCardError::Host);
    card.capacityBytes = card.extCsd.capacityBytes;
    attached = true;
    return true;
}

bool WarmCard::readExtCsd()
{
    u8 extCsd[512];
    if (!host->readBlocks(kCmd8SendExtCsd, 0, extCsd, 1)) return fail(WarmCardError::Host);
    if (!decodeExtCsd(extCsd, sizeof extCsd, &card.extCsd)) return fail(WarmCardError::InvalidExtCsd);
    return true;
}

bool WarmCard::switchExtCsd(u8 index, u8 value)
{
    u32 response[4];
    // CMD6 access 3 (write byte): the card answers R1b and reports SWITCH_ERROR in the next status.
    return host->command(kCmd6Switch, (3u << 24) | (static_cast<u32>(index) << 16) | (static_cast<u32>(value) << 8),
                         ResponseType::R1Busy, response) &&
           host->waitCardReady(card.relativeAddress, kStateTransfer, kProgramPolls);
}

bool WarmCard::recover()
{
    if (!attached || !host) return fail(WarmCardError::InvalidArgument);
    if (!host->start()) return fail(WarmCardError::Host);
    u32 response[4];
    // CMD12 is illegal in the transfer state; the card only flags it in the next status, so its
    // failure says nothing and the state wait decides.
    host->command(kCmd12StopTransmission, 0, ResponseType::R1Busy, response);
    if (!host->waitCardReady(card.relativeAddress, kStateTransfer, kProgramPolls)) return fail(WarmCardError::Host);
    return true;
}

bool WarmCard::upgrade(const BusControl &bus, u32 highSpeedHz)
{
    if (!ready() || !bus.setClock || !bus.setBusWidth) return fail(WarmCardError::InvalidArgument);
    // BUS_WIDTH is write-only (it reads back 0 on hardware), so the 8-bit bus is proved the way
    // Linux's bus test does it: EXT_CSD read through the new width, CRC-checked by the host,
    // must repeat the read-only fields of the 1-bit read.
    const ExtCsdInfo legacy = card.extCsd;
    if (!switchExtCsd(kExtCsdBusWidth, 2)) return fail(WarmCardError::Host);
    if (!bus.setBusWidth(bus.context, 8)) return fail(WarmCardError::BusMode);
    if (!readExtCsd()) return false;
    if (card.extCsd.capacityBytes != legacy.capacityBytes || card.extCsd.revision != legacy.revision ||
        card.extCsd.deviceType != legacy.deviceType || card.extCsd.sectorBytes != legacy.sectorBytes) {
        return fail(WarmCardError::BusMode);
    }
    card.busWidth = 8;
    if ((card.extCsd.deviceType & 2) && highSpeedHz > 26000000) {
        if (!switchExtCsd(kExtCsdHsTiming, 1)) return fail(WarmCardError::Host);
        if (!bus.setClock(bus.context, highSpeedHz)) return fail(WarmCardError::BusClock);
        if (!readExtCsd()) return false;
        if (card.extCsd.hsTiming != 1) return fail(WarmCardError::BusMode);
        card.clockHz = highSpeedHz;
    }
    return true;
}

bool WarmCard::readBlocks(u64 lba, u32 count, u8 *destination)
{
    const u64 sectors = card.capacityBytes / 512;
    if (!ready() || !destination || !count || count > kMaxTransferBlocks || lba >= sectors ||
        count > sectors - lba || lba > 0xffffffffULL) {
        return false;
    }
    const bool ok = count == 1 ? host->readBlocks(kCmd17ReadSingleBlock, static_cast<u32>(lba), destination, 1)
                               : host->readBlocks(kCmd18ReadMultiple, static_cast<u32>(lba), destination, count);
    if (!ok) return fail(WarmCardError::Host);
    return true;
}

bool WarmCard::readBlocksDma(u64 lba, u32 count, u32 tablePhys)
{
    const u64 sectors = card.capacityBytes / 512;
    if (!ready() || !tablePhys || !count || count > kMaxDmaBlocks || lba >= sectors || count > sectors - lba ||
        lba > 0xffffffffULL) {
        return false;
    }
    // A failed DMA read leaves the card attached (unlike fail()): the caller recovers it and reads by PIO.
    if (!host->readBlocksDma(count == 1 ? kCmd17ReadSingleBlock : kCmd18ReadMultiple, static_cast<u32>(lba),
                             tablePhys, count)) {
        lastError = WarmCardError::Host;
        return false;
    }
    return true;
}

void WarmCard::allowWrites(u64 firstLba, u64 lastLba)
{
    const u64 sectors = card.capacityBytes / 512;
    if (!sectors || lastLba < firstLba || lastLba >= sectors || !lastLba) {
        writeFirst = 1;
        writeLast = 0;
        return;
    }
    writeFirst = firstLba;
    writeLast = lastLba;
}

bool WarmCard::writeBlocks(u64 lba, u32 count, const u8 *source)
{
    if (!ready() || !source || !count || count > kMaxTransferBlocks) return false;
    // A refused write is the caller's mistake, not a card failure: report it without detaching.
    if (!writable() || lba < writeFirst || lba > writeLast || count - 1 > writeLast - lba) {
        lastError = WarmCardError::WriteRefused;
        return false;
    }
    const bool ok = count == 1 ? host->writeBlocks(kCmd24WriteSingleBlock, static_cast<u32>(lba), source, 1)
                               : host->writeBlocks(kCmd25WriteMultiple, static_cast<u32>(lba), source, count);
    if (!ok || !host->waitCardReady(card.relativeAddress, kStateTransfer, kProgramPolls)) {
        return fail(WarmCardError::Host);
    }
    return true;
}

const char *hostErrorName(HostError error)
{
    switch (error) {
    case HostError::None: return "none";
    case HostError::InvalidArgument: return "invalid argument";
    case HostError::NotReady: return "not ready";
    case HostError::ResetTimeout: return "reset timeout";
    case HostError::InhibitTimeout: return "inhibit timeout";
    case HostError::CommandTimeout: return "command timeout";
    case HostError::DataTimeout: return "data timeout";
    case HostError::Controller: return "controller error";
    case HostError::CardStatus: return "card status error";
    case HostError::CardState: return "unexpected card state";
    }
    return "unknown";
}

}
