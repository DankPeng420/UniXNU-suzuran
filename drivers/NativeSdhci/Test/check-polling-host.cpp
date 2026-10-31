#include "../PollingHost.h"
#include "../SdhciRegs.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace sdhci;

namespace {
unsigned checks = 0;
unsigned failures = 0;

void expect(bool condition, const char *name)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

enum class Fault { None, Command, Buffer, Word, Transfer };

struct Command {
    u8 index;
    u32 argument;
    u16 flags;
    u16 transfer;
};

struct Write {
    u32 offset;
    u32 value;
    u32 width;
};

struct FakeHost {
    std::array<u8, 512> extCsd = {};
    std::array<u8, 512> data = {};
    std::array<u8, 512> pending = {};
    std::map<u32, std::array<u8, 512>> written;
    u32 blocksDone = 0;
    u32 blocksTotal = 0;
    u32 hostWidth = 1;
    bool dataWrite = false;
    bool widthMismatch = false;
    std::array<u32, 4> response = {};
    std::vector<Command> commands;
    std::vector<Write> writes;
    u32 status = 0;
    u32 statusEnable = 0;
    u32 signalEnable = 0xffffffff;
    u32 argument = 0;
    u32 state = 4;
    u32 responseErrors = 0;
    u32 errorBits = kErrDataCrc;
    u32 cardSectorCount = 0x04000000;
    u32 polls = 0;
    u32 commandAfter = 2;
    u32 bufferAfter = 4;
    u32 completionAfter = 3;
    u32 completionPolls = 0;
    u32 wordCount = 0;
    u32 faultWord = 7;
    u32 resetPolls = 0;
    u32 resetCount = 0;
    u32 delayCount = 0;
    u32 inhibit = 0;
    u32 presentDelay = 0;
    u32 busyPolls = 0;
    u32 readyCardAfter = 0;
    u32 ocrBusyPolls = 0;
    u8 loseCompletion = 0xff;
    bool loseKeepsEffect = false;
    u32 ocrPolls = 0;
    bool byteMode = false;
    u16 rca = 1;
    u16 transfer = 0;
    u8 hostControl1 = 0;
    u32 admaLow = 0;
    u32 admaHigh = 0xffffffff;
    u32 dmaBlocks = 0;
    bool admaFault = false;
    u16 blockSize = 0;
    u16 blockCount = 0;
    u8 activeCommand = 0;
    u8 reset = 0;
    bool active = false;
    bool hasData = false;
    bool busyResponse = false;
    bool noResponse = false;
    bool commandEmitted = false;
    bool bufferEmitted = false;
    bool transferEmitted = false;
    bool errorEmitted = false;
    bool stuckReset = false;
    bool missingBuffer = false;
    bool missingTransfer = false;
    bool badCsd = false;
    bool wrongCsdState = false;
    bool unexpectedIndexCheck = false;
    bool badTransferMode = false;
    bool rejectedWrite = false;
    bool staleTransfer = false;
    Fault fault = Fault::None;
    u8 onlyFaultCommand = 0xff;

    FakeHost()
    {
        extCsd[192] = 7;
        extCsd[194] = 2;
        setSectors(cardSectorCount);
    }

    void setSectors(u32 sectors)
    {
        cardSectorCount = sectors;
        for (u32 byte = 0; byte < 4; ++byte) extCsd[212 + byte] = static_cast<u8>(sectors >> (8 * byte));
    }

    std::array<u8, 512> blockBytes(u32 lba) const
    {
        const auto found = written.find(lba);
        if (found != written.end()) return found->second;
        std::array<u8, 512> bytes;
        for (u32 byte = 0; byte < 512; ++byte) bytes[byte] = static_cast<u8>(byte * 13 + lba * 7);
        if (lba == 1) std::memcpy(bytes.data(), "EFI PART", 8);
        return bytes;
    }

    void finishBlock()
    {
        ++blocksDone;
        wordCount = 0;
        if (blocksDone < blocksTotal) {
            bufferEmitted = false;
            polls = 0;
        }
    }

    bool hasFault(Fault phase) const
    {
        return fault == phase && (onlyFaultCommand == 0xff || onlyFaultCommand == activeCommand);
    }

    void emit(u32 bits)
    {
        status |= bits & statusEnable;
    }

    void tick()
    {
        if (!active) return;
        ++polls;
        if (hasFault(Fault::Command) && !errorEmitted && polls >= commandAfter) {
            errorEmitted = true;
            emit(errorBits);
        }
        if (!noResponse && !commandEmitted && polls >= commandAfter) {
            commandEmitted = true;
            emit(kIntCommandComplete);
        }
        if (hasData && (transfer & kModeDmaEnable) && commandEmitted && blocksDone < blocksTotal) {
            if (admaFault) {
                emit(kErrAdma);
            } else {
                dmaBlocks += blocksTotal;
                blocksDone = blocksTotal;
            }
        }
        if (hasData && !(transfer & kModeDmaEnable) && commandEmitted && !bufferEmitted && polls >= bufferAfter &&
            !missingBuffer) {
            bufferEmitted = true;
            emit(dataWrite ? kIntBufferWriteReady : kIntBufferReadReady);
            if (hasFault(Fault::Buffer) || widthMismatch) emit(errorBits);
        }
        if ((hasData && blocksDone == blocksTotal) || (busyResponse && commandEmitted)) {
            ++completionPolls;
            if (hasFault(Fault::Transfer) && completionPolls >= completionAfter) emit(errorBits);
            if (!missingTransfer && !transferEmitted && completionPolls >= completionAfter) {
                transferEmitted = true;
                emit(kIntTransferComplete);
            }
        }
    }

    void launch(u16 value)
    {
        activeCommand = static_cast<u8>(value >> 8);
        const u16 flags = value & 0xff;
        commands.push_back({activeCommand, argument, flags, transfer});
        active = true;
        hasData = (flags & kCmdDataPresent) != 0;
        busyResponse = (flags & 3) == kCmdResponse48Busy;
        noResponse = false;
        commandEmitted = bufferEmitted = transferEmitted = errorEmitted = false;
        polls = wordCount = completionPolls = blocksDone = 0;
        blocksTotal = hasData ? blockCount : 0;
        dataWrite = activeCommand == kCmd24WriteSingleBlock || activeCommand == kCmd25WriteMultiple;
        widthMismatch = hasData && ((extCsd[183] == 2) != (hostWidth == 8));
        response = {};
        const bool lose = activeCommand == loseCompletion;
        if (lose && !loseKeepsEffect) {
        } else if (activeCommand == kCmd0GoIdleState) {
            state = 0;
            rca = 0;
            ocrPolls = 0;
            extCsd[183] = extCsd[185] = 0;
        } else if (activeCommand == kCmd1SendOpCond) {
            noResponse = state > 1;
            const bool ready = ++ocrPolls > ocrBusyPolls;
            if (ready) state = 1;
            response[0] = (byteMode ? 0x00ff8080u : 0x40ff8080u) | (ready ? 0x80000000u : 0u);
        } else if (activeCommand == kCmd2AllSendCid) {
            noResponse = state != 1;
            state = 2;
            response = {0x11223344, 0x55667788, 0x99aabbcc, 0x00ddeeff};
        } else if (activeCommand == kCmd3SetRelativeAddr) {
            noResponse = state != 2;
            rca = static_cast<u16>(argument >> 16);
            response[0] = 2u << 9;
            state = 3;
        } else if (activeCommand == kCmd13SendStatus) {
            noResponse = (argument >> 16) != rca;
            const bool ready = ++busyPolls >= readyCardAfter;
            response[0] = (state << 9) | (ready ? 0x100 : 0) | responseErrors;
        } else if (activeCommand == kCmd7SelectCard) {
            state = argument ? 4 : 3;
            busyPolls = 0;
            response[0] = 0x100 | (state << 9) | responseErrors;
        } else if (activeCommand == kCmd9SendCsd) {
            wrongCsdState |= state != 3;
            unexpectedIndexCheck |= (flags & kCmdIndexCheck) != 0;
            const u32 csd[4] = {badCsd ? 0x10000000u : 0xd0000000u,
                                 0x000903ff, 0xc0038000, 0};
            for (u32 word = 0; word < 4; ++word) {
                response[word] = csd[3 - word] >> 8;
                if (word != 3) response[word] |= csd[2 - word] << 24;
            }
        } else if (activeCommand == kCmd6Switch) {
            if ((argument >> 24) == 3) extCsd[(argument >> 16) & 0xff] = static_cast<u8>(argument >> 8);
            response[0] = 0x900 | responseErrors;
        } else if (hasData) {
            const bool multi = activeCommand == kCmd18ReadMultiple || activeCommand == kCmd25WriteMultiple;
            const u16 expected = kModeBlockCountEnable | (dataWrite ? 0 : kModeDirectionRead) |
                                 (multi ? kModeMultiBlock | kModeAutoCmd12 : 0);
            badTransferMode |= (transfer & ~kModeDmaEnable) != expected || blockSize != 512 ||
                               (multi ? blockCount < 2 : blockCount != 1) ||
                               ((transfer & kModeDmaEnable) && (dataWrite || (hostControl1 & kHostDmaSelectMask) !=
                                                                kHostDmaSelectAdma2 || !admaLow || admaHigh));
            response[0] = 0x900 | responseErrors;
            data = blockBytes(argument);
        }
        if (lose) {
            noResponse = true;
            loseCompletion = 0xff;
        }
        if (staleTransfer) status |= kIntTransferComplete;
    }

    static u8 read8(void *context, u32 offset)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        if (offset == kRegHostControl1) return fake.hostControl1;
        expect(offset == kRegSoftwareReset, "only software-reset and host-control bytes are read by core");
        if (!fake.stuckReset && ++fake.resetPolls >= 2) fake.reset = 0;
        return fake.reset;
    }

    static u32 read32(void *context, u32 offset)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        if (offset == kRegInterruptStatus) {
            fake.tick();
            return fake.status;
        }
        if (offset == kRegPresentState) {
            if (fake.presentDelay) {
                --fake.presentDelay;
                return kPresentCmdInhibit | kPresentDataInhibit;
            }
            const u32 buffer = fake.dataWrite ? kPresentBufferWrite : kPresentBufferRead;
            return fake.inhibit | (fake.bufferEmitted && fake.wordCount < 128 ? buffer : 0u);
        }
        if (offset >= kRegResponse && offset < kRegResponse + 16) return fake.response[(offset - kRegResponse) / 4];
        if (offset == kRegBufferPort) {
            if (fake.dataWrite || !fake.bufferEmitted || fake.wordCount >= 128) {
                expect(false, "buffer is read only while a block is ready");
                return 0;
            }
            auto bytes = fake.activeCommand == kCmd8SendExtCsd ? fake.extCsd
                                                               : fake.blockBytes(fake.argument + fake.blocksDone);
            if (fake.activeCommand == kCmd8SendExtCsd) bytes[183] = 0;   // BUS_WIDTH is write-only
            u32 value = 0;
            for (u32 byte = 0; byte < 4; ++byte) value |= static_cast<u32>(bytes[fake.wordCount * 4 + byte]) << (byte * 8);
            if (fake.hasFault(Fault::Word) && fake.blocksDone == 0 && fake.wordCount == fake.faultWord) fake.emit(fake.errorBits);
            if (++fake.wordCount == 128) fake.finishBlock();
            return value;
        }
        expect(false, "no unknown MMIO read");
        return 0;
    }

    static void write8(void *context, u32 offset, u8 value)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        fake.writes.push_back({offset, value, 1});
        if (offset == kRegSoftwareReset) {
            fake.rejectedWrite |= value != (kResetCommand | kResetData);
            fake.reset = value;
            fake.resetPolls = 0;
            ++fake.resetCount;
            fake.active = false;
        } else if (offset == kRegTimeoutControl) {
            fake.rejectedWrite |= value != 0x0e;
        } else if (offset == kRegHostControl1) {
            fake.rejectedWrite |= (value & ~kHostDmaSelectMask) != (fake.hostControl1 & ~kHostDmaSelectMask);
            fake.hostControl1 = value;
        } else {
            fake.rejectedWrite = true;
        }
    }

    static void write16(void *context, u32 offset, u16 value)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        fake.writes.push_back({offset, value, 2});
        if (offset == kRegBlockSize) fake.blockSize = value;
        else if (offset == kRegBlockCount) fake.blockCount = value;
        else if (offset == kRegTransferMode) fake.transfer = value;
        else if (offset == kRegCommand) fake.launch(value);
        else fake.rejectedWrite = true;
    }

    static void write32(void *context, u32 offset, u32 value)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        fake.writes.push_back({offset, value, 4});
        if (offset == kRegArgument) fake.argument = value;
        else if (offset == kRegBufferPort && fake.dataWrite && fake.bufferEmitted && fake.wordCount < 128) {
            for (u32 byte = 0; byte < 4; ++byte) fake.pending[fake.wordCount * 4 + byte] = static_cast<u8>(value >> (8 * byte));
            if (++fake.wordCount == 128) {
                fake.written[fake.argument + fake.blocksDone] = fake.pending;
                fake.finishBlock();
            }
        }
        else if (offset == kRegAdmaAddress) fake.admaLow = value;
        else if (offset == kRegAdmaAddress + 4) fake.admaHigh = value;
        else if (offset == kRegInterruptStatus) fake.status &= ~value;
        else if (offset == kRegInterruptEnable) fake.statusEnable = value;
        else if (offset == kRegInterruptSignal) fake.signalEnable = value;
        else fake.rejectedWrite = true;
    }

    static void delay(void *context, u32)
    {
        ++static_cast<FakeHost *>(context)->delayCount;
    }

    RegisterIo io()
    {
        return {this, read8, read32, write8, write16, write32, delay};
    }
};

PollLimits shortLimits()
{
    return {10, 30, 10, 1, 4};
}

void configured(FakeHost &fake, PollingHost &host)
{
    expect(host.configure(fake.io(), shortLimits()), "valid MMIO callbacks configure");
}

void testStartup()
{
    FakeHost fake;
    PollingHost host;
    configured(fake, host);
    fake.status = kIntTransferComplete | kIntBufferReadReady | kErrDataTimeout | kIntCardInterrupt;
    expect(host.start(), "warm controller starts");
    expect(host.ready(), "host is ready after reset acknowledgement");
    expect(fake.signalEnable == 0, "firmware IRQ signaling is disabled for polling");
    expect((fake.statusEnable & (kIntCommandComplete | kIntTransferComplete | kIntBufferReadReady)) ==
           (kIntCommandComplete | kIntTransferComplete | kIntBufferReadReady), "PIO status latching is enabled");
    expect(fake.status == kIntCardInterrupt, "only owned status is acknowledged");
    expect(!fake.rejectedWrite, "clock, power, bus width and vendor windows remain untouched");
    expect(!host.configure(fake.io()), "callbacks cannot be replaced while running");
    host.stop();
    expect(!host.ready(), "stop prevents new commands");

    FakeHost stuck;
    stuck.stuckReset = true;
    PollingHost failed;
    configured(stuck, failed);
    expect(!failed.start(), "stuck CMD/DAT reset fails start");
    expect(failed.result().error == HostError::ResetTimeout, "reset failure is diagnostic");
    expect(stuck.delayCount == shortLimits().resetPolls, "reset wait is bounded");
    u32 response[4];
    expect(!failed.command(13, 1u << 16, ResponseType::R1, response), "no command after failed reset");
    expect(stuck.commands.empty(), "failed reset never reaches command register");

    RegisterIo invalid = fake.io();
    invalid.read32 = nullptr;
    PollingHost missing;
    expect(!missing.configure(invalid), "missing callback is refused");
    expect(!missing.start(), "invalid IO never touches hardware");
    PollLimits invalidLimits = shortLimits();
    invalidLimits.transferPolls = 0;
    expect(!missing.configure(fake.io(), invalidLimits), "zero transfer budget is refused");
    invalidLimits = shortLimits();
    invalidLimits.commandPolls = 0xffffffff;
    expect(!missing.configure(fake.io(), invalidLimits), "overflow-prone poll budget is refused");
}

void testCommands()
{
    FakeHost fake;
    PollingHost host;
    configured(fake, host);
    expect(host.start(), "command host starts");
    fake.inhibit = kPresentDataInhibit;
    u32 response[4] = {};
    expect(host.command(13, 1u << 16, ResponseType::R1, response), "CMD13 can inspect a DAT-inhibited card");
    expect(response[0] == 0x900, "short R1 response is not R2-shifted");
    expect(fake.commands.back().flags == (kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck),
           "R1 has response CRC and index checks");
    fake.inhibit = 0;
    expect(host.command(7, 0, ResponseType::None, nullptr), "deselect does not expect an R1 timeout");
    expect(fake.commands.back().flags == 0, "CMD7 deselect carries no-response flags");
    expect(host.command(9, 1u << 16, ResponseType::R2, response), "standby card supplies CSD");
    expect(response[0] == 0xd0000000 && response[1] == 0x000903ff && response[2] == 0xc0038000,
           "controller R2 words become canonical CSD bit positions");
    expect(!fake.wrongCsdState && !fake.unexpectedIndexCheck, "CSD uses standby state without opcode check");
    expect(host.command(7, 1u << 16, ResponseType::R1Busy, response), "R1b waits for busy-end completion");
    expect(fake.transferEmitted, "R1b command completion alone is insufficient");
    expect(fake.commands.back().transfer == 0, "nondata command clears inherited DMA mode");
    fake.inhibit = kPresentCmdInhibit;
    const auto commandsBefore = fake.commands.size();
    expect(!host.command(13, 1u << 16, ResponseType::R1, response), "inhibited command times out");
    expect(host.result().error == HostError::InhibitTimeout, "inhibit has its own failure code");
    expect(fake.commands.size() == commandsBefore, "inhibited command is never issued");

    FakeHost bad;
    bad.responseErrors = 1u << 22;
    PollingHost badHost;
    configured(bad, badHost);
    expect(badHost.start(), "R1 error host starts");
    expect(!badHost.command(13, 1u << 16, ResponseType::R1, response), "illegal-command R1 is not success");
    expect(badHost.result().error == HostError::CardStatus, "R1 error is distinguished from controller error");
    expect((badHost.result().cardStatus & (1u << 22)) != 0, "full R1 status is preserved");

    FakeHost timed;
    timed.rca = 2;
    PollingHost timeout;
    configured(timed, timeout);
    expect(timeout.start(), "timeout host starts");
    expect(!timeout.command(13, 1u << 16, ResponseType::R1, response), "nonexistent RCA times out");
    expect(timeout.result().error == HostError::CommandTimeout, "missing response is a command timeout");
    expect(timed.polls == shortLimits().commandPolls, "command wait is bounded");
}

void testReads()
{
    for (u32 delay = 1; delay <= 12; ++delay) {
        FakeHost fake;
        fake.completionAfter = delay;
        fake.commandAfter = 1 + delay % 3;
        fake.bufferAfter = fake.commandAfter + 1 + delay % 4;
        PollingHost host;
        configured(fake, host);
        expect(host.start(), "read host starts");
        std::array<u8, 514> destination;
        destination.fill(0xa5);
        expect(host.readBlocks(17, 123, destination.data() + 1, 1), "delayed transfer completion succeeds");
        expect(!std::memcmp(destination.data() + 1, fake.data.data(), 512), "unaligned output matches all 512 FIFO bytes");
        expect(destination.front() == 0xa5 && destination.back() == 0xa5, "read preserves destination guards");
        expect(fake.blocksDone == 1 && fake.wordCount == 0, "PIO drains exactly one complete block");
        expect(host.result().bytesTransferred == 512, "verified byte count is published only on success");
        expect((fake.status & (kIntCommandComplete | kIntBufferReadReady | kIntTransferComplete)) == 0,
               "owned completion events are acknowledged");
        expect(!fake.badTransferMode && !fake.rejectedWrite, "read never enables DMA or writes a data sector");
    }

    for (Fault phase : {Fault::Command, Fault::Buffer, Fault::Word, Fault::Transfer}) {
        FakeHost fake;
        fake.fault = phase;
        PollingHost host;
        configured(fake, host);
        expect(host.start(), "fault-injection host starts");
        std::array<u8, 512> destination;
        destination.fill(0xa5);
        expect(!host.readBlocks(17, 1, destination.data(), 1), "controller fault fails the read");
        expect(host.result().error == HostError::Controller, "high error bit fails without summary ERROR bit");
        expect(host.result().interruptStatus & kErrDataCrc, "original controller error is retained");
        expect(host.result().bytesTransferred == 0, "failed read publishes zero bytes");
        expect(!host.ready(), "failed transfer prevents implicit reuse without reattachment");
        if (phase == Fault::Word) expect(fake.blocksDone == 1, "a mid-block error fails at the end of that block");
    }

    for (bool missingBuffer : {false, true}) {
        FakeHost fake;
        fake.missingBuffer = missingBuffer;
        fake.missingTransfer = !missingBuffer;
        PollingHost host;
        configured(fake, host);
        expect(host.start(), "stalled host starts");
        std::array<u8, 512> destination = {};
        expect(!host.readBlocks(17, 1, destination.data(), 1), "stalled data phase fails");
        expect(host.result().error == HostError::DataTimeout, "data timeout is diagnosed");
        expect(fake.delayCount <= 2 * shortLimits().transferPolls + shortLimits().commandPolls + 2,
               "the buffer and completion waits are each bounded");
        if (missingBuffer) expect(fake.wordCount == 0, "missing buffer event does not touch FIFO");
    }

    FakeHost invalid;
    PollingHost invalidHost;
    configured(invalid, invalidHost);
    expect(invalidHost.start(), "invalid-read host starts");
    std::array<u8, 512> destination = {};
    expect(!invalidHost.readBlocks(24, 1, destination.data(), 1), "PIO read interface refuses write command");
    expect(!invalidHost.readBlocks(17, 1, destination.data(), 2), "single-block command refuses a block count");
    expect(!invalidHost.readBlocks(18, 1, destination.data(), 1), "multi-block command needs two blocks");
    expect(!invalidHost.writeBlocks(17, 1, destination.data(), 1), "PIO write interface refuses read command");
    expect(invalid.commands.empty(), "invalid read never reaches hardware command register");
}

void testWarmCard()
{
    const u16 candidates[] = {1, 2, 0};
    FakeHost fake;
    fake.rca = 2;
    fake.transfer = 0x3f;
    fake.status = kIntTransferComplete;
    PollingHost host;
    configured(fake, host);
    WarmCard card;
    expect(card.attach(host, candidates, 3), "firmware-selected card attaches after skipping absent RCA");
    expect(card.ready() && card.info().relativeAddress == 2, "successful RCA is retained");
    expect(card.info().capacityBytes == (32ULL << 30), "capacity comes from EXT_CSD SEC_COUNT");
    expect(card.info().csd.structure == 3, "modern CSD is accepted through actual R2 MMIO path");
    expect(fake.resetCount == 2, "wrong-RCA command timeout is recovered before next candidate");
    expect(!fake.wrongCsdState && !fake.unexpectedIndexCheck, "warm CSD is read while deselected with correct R2 flags");
    expect(fake.state == 4 && card.info().extCsd.partitionAccess == 0, "card returns to selected user partition");
    expect(!fake.rejectedWrite && !fake.badTransferMode, "warm handoff preserves firmware clock and bus configuration");
    bool initializationCommand = false;
    bool extCsdRead = false;
    for (const Command &command : fake.commands) {
        initializationCommand |= command.index <= 3 || command.index == 6 ||
                                 command.index == 16 || command.index >= 24;
        extCsdRead |= command.index == 8;
    }
    expect(!initializationCommand && extCsdRead, "no reinitialization, partition switch or write is sent");
    std::array<u8, 512> gpt = {};
    expect(card.readBlock(1, gpt.data()), "attached card reads GPT sector");
    expect(!std::memcmp(gpt.data(), "EFI PART", 8), "GPT bytes traverse the real transport core");
    expect(card.readBlock(card.info().capacityBytes / 512 - 1, gpt.data()), "last sector is addressable");
    const auto commandsBefore = fake.commands.size();
    expect(!card.readBlock(card.info().capacityBytes / 512, gpt.data()), "one-past-capacity read is refused");
    expect(!card.readBlock(~0ULL, gpt.data()), "64-bit LBA cannot truncate into CMD17 argument");
    expect(fake.commands.size() == commandsBefore, "out-of-range access emits no command");
    fake.onlyFaultCommand = 17;
    fake.fault = Fault::Word;
    expect(!card.readBlock(1, gpt.data()), "attached medium fails a damaged transfer");
    expect(!card.ready(), "damaged transfer detaches card instead of serving stale data");
    card.detach();
    expect(!card.readBlock(1, gpt.data()), "detached card emits no read");

    for (u32 initialState : {3u, 4u}) {
        FakeHost standby;
        standby.state = initialState;
        PollingHost standbyHost;
        configured(standby, standbyHost);
        WarmCard standbyCard;
        expect(standbyCard.attach(standbyHost, candidates, 1), "standby and transfer handoffs both attach");
    }

    for (u32 mutation = 0; mutation < 10; ++mutation) {
        FakeHost broken;
        WarmCardError expected = WarmCardError::UnsupportedCard;
        switch (mutation) {
        case 0: broken.extCsd[179] = 1; expected = WarmCardError::WrongPartition; break;
        case 1: broken.extCsd[179] = 3; expected = WarmCardError::WrongPartition; break;
        case 2: broken.extCsd[61] = 1; break;
        case 3: broken.extCsd[192] = 1; break;
        case 4: broken.setSectors(0); break;
        case 5: broken.extCsd[15] = 1; expected = WarmCardError::CommandQueue; break;
        case 6: broken.extCsd[194] = 3; expected = WarmCardError::InvalidExtCsd; break;
        case 7: broken.badCsd = true; expected = WarmCardError::InvalidCsd; break;
        case 8: broken.state = 5; expected = WarmCardError::UnexpectedState; break;
        case 9: broken.setSectors(0x00400000); break;
        }
        PollingHost brokenHost;
        configured(broken, brokenHost);
        WarmCard brokenCard;
        expect(!brokenCard.attach(brokenHost, candidates, 1), "unsupported card handoff is rejected");
        expect(brokenCard.error() == expected, "specific attach failure is retained");
        expect(!brokenCard.ready(), "unsupported medium is not published");
        std::array<u8, 512> bytes = {};
        const auto before = broken.commands.size();
        expect(!brokenCard.readBlock(1, bytes.data()), "unsupported medium cannot read another partition");
        expect(broken.commands.size() == before, "unsupported medium emits no follow-up I/O");
    }
}

struct FakeClock {
    std::vector<u32> rates;
    std::vector<u32> widths;
    FakeHost *card = nullptr;    // the host side of the bus width, when a test models it
    bool refuse = false;
    bool stuckWidth = false;     // a host that accepts the width request but stays on one line

    static bool set(void *context, u32 hz)
    {
        auto &clock = *static_cast<FakeClock *>(context);
        clock.rates.push_back(hz);
        return !clock.refuse;
    }

    static bool width(void *context, u32 bits)
    {
        auto &clock = *static_cast<FakeClock *>(context);
        clock.widths.push_back(bits);
        if (clock.card && !clock.stuckWidth) clock.card->hostWidth = bits;
        return !clock.refuse;
    }

    BusControl bus() { return {this, set, width}; }
};

void testColdCard()
{
    const u16 candidates[] = {1, 2};
    FakeHost fake;
    fake.rca = 0;
    fake.ocrBusyPolls = 3;
    PollingHost host;
    configured(fake, host);
    WarmCard card;
    expect(!card.attach(host, candidates, 2), "a card that answers no RCA does not attach warm");
    expect(card.error() == WarmCardError::NoRelativeAddress, "silent warm card is reported as no RCA");
    const auto warmCommands = fake.commands.size();
    FakeClock clock;
    expect(card.initialize(host, 2, clock.bus(), 25000000), "cold identification attaches the card");
    expect(card.ready() && card.info().relativeAddress == 2 && card.info().cold, "cold card keeps the assigned RCA");
    expect(card.info().ocr == 0xc0ff8080u, "ready OCR is retained");
    expect(card.info().cid[0] != 0, "CID is retained");
    expect(clock.rates == std::vector<u32>({400000, 25000000}), "identification runs at 400 kHz, transfer at the asked rate");
    expect(clock.widths == std::vector<u32>({1}), "identification starts on one data line");
    std::vector<u8> sequence;
    for (auto command = fake.commands.begin() + warmCommands; command != fake.commands.end(); ++command) {
        sequence.push_back(command->index);
        if (command->index == kCmd1SendOpCond) {
            expect(command->argument == 0x40ff8080u, "CMD1 asks for sector mode at both voltage windows");
            expect(command->flags == kCmdResponse48, "R3 carries neither CRC nor index check");
        }
        if (command->index == kCmd3SetRelativeAddr) expect(command->argument == 2u << 16, "CMD3 assigns the RCA");
    }
    expect(sequence == std::vector<u8>({0, 1, 1, 1, 1, 2, 3, 13, 9, 7, 13, 8, 13}),
           "CMD0, CMD1 until ready, CMD2, CMD3, then the shared standby/select/EXT_CSD tail");
    expect(card.info().capacityBytes == (32ULL << 30), "cold capacity comes from EXT_CSD");
    std::array<u8, 512> gpt = {};
    expect(card.readBlock(1, gpt.data()), "cold card reads the GPT sector");
    expect(!std::memcmp(gpt.data(), "EFI PART", 8), "cold GPT bytes are intact");
    expect(!fake.rejectedWrite, "the cold path changes the clock only through the board callback");
    const CommandTrace *lastCommand = host.recent(0);
    expect(lastCommand && lastCommand->index == kCmd17ReadSingleBlock && lastCommand->argument == 1 &&
           (lastCommand->status & kIntCommandComplete), "trace keeps the last command and its completion");
    expect(host.recent(1) && host.recent(1)->index == kCmd13SendStatus, "trace is ordered newest first");
    expect(!host.recent(8), "trace depth is bounded");

    for (bool keepsEffect : {false, true}) {
        FakeHost lost;
        lost.loseCompletion = kCmd3SetRelativeAddr;
        lost.loseKeepsEffect = keepsEffect;
        PollingHost lostHost;
        configured(lost, lostHost);
        FakeClock lostClock;
        WarmCard lostCard;
        expect(lostCard.initialize(lostHost, 2, lostClock.bus(), 25000000),
               keepsEffect ? "a CMD3 the card took but the host lost is confirmed by CMD13"
                           : "a lost CMD3 is sent again after a command-line reset");
        expect(lostCard.ready() && lostCard.info().relativeAddress == 2, "recovered card keeps its RCA");
    }

    FakeHost refused;
    PollingHost refusedHost;
    configured(refused, refusedHost);
    FakeClock broken;
    broken.refuse = true;
    WarmCard refusedCard;
    expect(!refusedCard.initialize(refusedHost, 2, broken.bus(), 25000000), "a refused bus clock fails identification");
    expect(refusedCard.error() == WarmCardError::BusClock, "clock failure is specific");
    expect(refused.commands.empty(), "no command is sent at an unknown clock");

    FakeHost busy;
    busy.ocrBusyPolls = 0xffffffff;
    PollingHost busyHost;
    configured(busy, busyHost);
    FakeClock busyClock;
    WarmCard busyCard;
    expect(!busyCard.initialize(busyHost, 2, busyClock.bus(), 25000000), "a card that never leaves busy fails");
    expect(busyCard.error() == WarmCardError::OperatingCondition, "OCR busy is specific");
    expect(busy.ocrPolls == 1000, "OCR polling is bounded");
    const CommandTrace *busyTrace = busyHost.recent(0);
    expect(busyTrace && busyTrace->index == kCmd1SendOpCond && busyTrace->response == 0x40ff8080u,
           "trace records the busy OCR");

    FakeHost byteMode;
    byteMode.byteMode = true;
    PollingHost byteHost;
    configured(byteMode, byteHost);
    FakeClock byteClock;
    WarmCard byteCard;
    expect(!byteCard.initialize(byteHost, 2, byteClock.bus(), 25000000), "byte-addressed card is refused");
    expect(byteCard.error() == WarmCardError::UnsupportedCard, "byte mode is unsupported");

    FakeHost invalid;
    PollingHost invalidHost;
    configured(invalid, invalidHost);
    FakeClock invalidClock;
    WarmCard invalidCard;
    expect(!invalidCard.initialize(invalidHost, 0, invalidClock.bus(), 25000000), "RCA 0 is refused");
    expect(!invalidCard.initialize(invalidHost, 2, invalidClock.bus(), 52000000), "high-speed rate needs HS timing first");
    expect(invalidCard.error() == WarmCardError::InvalidArgument && invalidClock.rates.empty(),
           "invalid arguments touch nothing");
}
}

void coldCard(FakeHost &fake, PollingHost &host, FakeClock &clock, WarmCard &card)
{
    configured(fake, host);
    clock.card = &fake;
    expect(card.initialize(host, 2, clock.bus(), 25000000), "cold card for the data tests");
}

void testMultiBlockAndWrites()
{
    FakeHost fake;
    PollingHost host;
    FakeClock clock;
    WarmCard card;
    coldCard(fake, host, clock, card);
    std::vector<u8> bytes(5 * 512, 0xa5);
    expect(card.readBlocks(40, 5, bytes.data()), "five blocks read with one CMD18");
    expect(fake.commands.back().index == kCmd18ReadMultiple && fake.blockCount == 5, "multi-block read uses CMD18 with a block count");
    bool same = true;
    for (u32 block = 0; block < 5; ++block) same &= !std::memcmp(bytes.data() + block * 512, fake.blockBytes(40 + block).data(), 512);
    expect(same, "every block of a multi-block read lands in order");
    expect(!fake.badTransferMode, "multi-block transfer mode carries Auto CMD12");
    expect(!card.readBlocks(fake.cardSectorCount - 2, 3, bytes.data()), "a read past the end is refused");
    expect(!card.readBlocks(0, kMaxTransferBlocks + 1, bytes.data()), "a read above the burst limit is refused");

    std::vector<u8> source(3 * 512);
    for (u32 index = 0; index < source.size(); ++index) source[index] = static_cast<u8>(index * 31 + 7);
    const auto before = fake.commands.size();
    expect(!card.writable() && !card.writeBlocks(100, 1, source.data()), "writes are refused until a window is opened");
    expect(card.error() == WarmCardError::WriteRefused && card.ready(), "a refused write keeps the card attached");
    card.allowWrites(100, 199);
    expect(card.writable(), "an open window makes the card writable");
    expect(!card.writeBlocks(99, 1, source.data()), "a write below the window is refused");
    expect(!card.writeBlocks(198, 3, source.data()), "a write crossing the window end is refused");
    expect(fake.commands.size() == before, "refused writes emit no command");
    expect(card.writeBlocks(100, 3, source.data()), "a write inside the window succeeds");
    expect(fake.commands.size() > before && fake.written.size() == 3, "CMD25 stores three blocks");
    bool stored = true;
    for (u32 block = 0; block < 3; ++block) {
        const auto found = fake.written.find(100 + block);
        stored &= found != fake.written.end() && !std::memcmp(found->second.data(), source.data() + block * 512, 512);
    }
    expect(stored, "written blocks hold the source bytes in order");
    expect(card.writeBlocks(199, 1, source.data()), "the last block of the window is writable with CMD24");
    bool cmd24 = false, cmd25 = false;
    for (const Command &command : fake.commands) {
        cmd24 |= command.index == kCmd24WriteSingleBlock;
        cmd25 |= command.index == kCmd25WriteMultiple;
    }
    expect(cmd24 && cmd25, "single and multiple block writes use CMD24 and CMD25");
    std::vector<u8> back(3 * 512);
    expect(card.readBlocks(100, 3, back.data()) && back == std::vector<u8>(source.begin(), source.end()),
           "written blocks read back");
    card.allowWrites(10, 5);
    expect(!card.writable(), "an inverted window closes writes");
    card.allowWrites(0, fake.cardSectorCount);
    expect(!card.writable(), "a window past the medium closes writes");

    // The panic path: reset the lines, stop the card, write again.
    card.allowWrites(100, 199);
    const auto beforeRecover = fake.commands.size();
    expect(card.recover(), "recover resets the host and returns the card to transfer state");
    bool stopped = false;
    for (auto at = beforeRecover; at < fake.commands.size(); ++at) stopped |= fake.commands[at].index == kCmd12StopTransmission;
    expect(stopped, "recover sends CMD12");
    expect(card.writeBlocks(150, 2, source.data()) && fake.written.count(151), "a write after recover succeeds");
    WarmCard detached;
    expect(!detached.recover(), "recover refuses a card that was never attached");
}

void testUpgrade()
{
    FakeHost fake;
    fake.extCsd[196] = 0x03;
    PollingHost host;
    FakeClock clock;
    WarmCard card;
    coldCard(fake, host, clock, card);
    expect(card.upgrade(clock.bus(), 52000000), "8-bit bus and high-speed timing are proved");
    expect(clock.widths == std::vector<u32>({1, 8}) && card.info().busWidth == 8, "the host follows the card to 8 bits");
    expect(clock.rates.back() == 52000000 && card.info().clockHz == 52000000, "high speed raises the clock");
    expect(fake.extCsd[183] == 2 && fake.extCsd[185] == 1, "CMD6 wrote BUS_WIDTH and HS_TIMING");
    std::vector<u8> bytes(2 * 512);
    expect(card.readBlocks(1, 2, bytes.data()) && !std::memcmp(bytes.data(), "EFI PART", 8), "reads work in the new bus mode");

    FakeHost legacy;
    legacy.extCsd[196] = 0x01;
    PollingHost legacyHost;
    FakeClock legacyClock;
    WarmCard legacyCard;
    coldCard(legacy, legacyHost, legacyClock, legacyCard);
    expect(legacyCard.upgrade(legacyClock.bus(), 52000000), "a 26 MHz-only card still gets the 8-bit bus");
    expect(legacy.extCsd[185] == 0 && legacyClock.rates.back() == 25000000, "no high-speed switch without DEVICE_TYPE bit 1");

    FakeHost stuck;
    stuck.extCsd[196] = 0x03;
    PollingHost stuckHost;
    FakeClock stuckClock;
    WarmCard stuckCard;
    coldCard(stuck, stuckHost, stuckClock, stuckCard);
    stuckClock.stuckWidth = true;
    expect(!stuckCard.upgrade(stuckClock.bus(), 52000000), "a host left on one line fails the EXT_CSD proof");
    expect(!stuckCard.ready(), "a failed upgrade detaches the card for a fresh initialize");
    stuckClock.stuckWidth = false;
    expect(stuckCard.initialize(stuckHost, 2, stuckClock.bus(), 25000000), "CMD0 recovers the legacy bus");
    expect(stuck.extCsd[183] == 0 && stuck.hostWidth == 1, "the retry is back on one line");
}

void testDmaReads()
{
    FakeHost fake;
    fake.hostControl1 = kHostDataWidth8 | kHostHighSpeed;
    PollingHost host;
    configured(fake, host);
    expect(host.start(), "DMA host starts");
    expect(host.readBlocksDma(kCmd18ReadMultiple, 100, 0x1000, 8), "ADMA2 multi-block read completes");
    expect(fake.dmaBlocks == 8 && fake.admaLow == 0x1000 && fake.admaHigh == 0, "descriptor table address is programmed");
    expect(fake.commands.back().transfer == (kModeBlockCountEnable | kModeDirectionRead | kModeMultiBlock |
                                             kModeAutoCmd12 | kModeDmaEnable), "ADMA2 read sets DMA enable");
    expect((fake.hostControl1 & kHostDmaSelectMask) == kHostDmaSelectAdma2 &&
           (fake.hostControl1 & (kHostDataWidth8 | kHostHighSpeed)) == (kHostDataWidth8 | kHostHighSpeed),
           "ADMA2 is selected without disturbing bus width and timing");
    expect(host.result().bytesTransferred == 8 * 512, "DMA read reports its bytes");
    expect(host.readBlocksDma(kCmd17ReadSingleBlock, 7, 0x2000, 1), "ADMA2 single-block read completes");
    expect(host.readBlocksDma(kCmd18ReadMultiple, 4096, 0x1000, kMaxDmaBlocks), "a 512 KiB ADMA2 read completes");
    expect(!host.readBlocksDma(kCmd18ReadMultiple, 4096, 0x1000, kMaxDmaBlocks + 1), "DMA reads stop at kMaxDmaBlocks");
    expect(host.start(), "host restarts after the oversize refusal");
    expect(!fake.badTransferMode && !fake.rejectedWrite, "DMA reads use well-formed transfer modes");

    std::vector<u8> bytes(2 * 512);
    expect(host.readBlocks(kCmd18ReadMultiple, 1, bytes.data(), 2) && !std::memcmp(bytes.data(), "EFI PART", 8),
           "PIO still works after DMA reads");
    expect(!(fake.commands.back().transfer & kModeDmaEnable), "PIO read does not inherit DMA enable");

    expect(!host.readBlocksDma(kCmd18ReadMultiple, 1, 0, 8), "no table is refused");
    expect(!host.readBlocksDma(kCmd18ReadMultiple, 1, 0x1002, 8), "unaligned table is refused");
    expect(!host.readBlocksDma(kCmd18ReadMultiple, 1, 0x1000, 1), "CMD18 with one block is refused");
    expect(!host.readBlocksDma(kCmd25WriteMultiple, 1, 0x1000, 8), "DMA writes are refused");

    expect(!host.ready(), "a refused request stops the host until it is started again");
    expect(host.start(), "host restarts after refused requests");
    fake.admaFault = true;
    expect(!host.readBlocksDma(kCmd18ReadMultiple, 100, 0x1000, 8), "ADMA error fails the read");
    expect(host.result().error == HostError::Controller, "ADMA error is a controller error");

    FakeHost cardFake;
    PollingHost cardHost;
    FakeClock clock;
    WarmCard card;
    coldCard(cardFake, cardHost, clock, card);
    expect(card.readBlocksDma(1, 2, 0x1000), "card-level ADMA2 read works");
    expect(cardFake.commands.back().index == kCmd18ReadMultiple && cardFake.commands.back().argument == 1,
           "card-level DMA read is CMD18 at the LBA");
    expect(!card.readBlocksDma(1, 2, 0), "card refuses a DMA read without a table");
    cardFake.admaFault = true;
    expect(!card.readBlocksDma(1, 2, 0x1000), "card-level ADMA error fails the read");
    expect(card.error() == WarmCardError::Host, "the DMA failure is a host error");
    cardFake.admaFault = false;
    expect(card.recover(), "the card stays attached after a DMA failure and recovers");
    std::fill(bytes.begin(), bytes.end(), 0);
    expect(card.readBlocks(1, 2, bytes.data()) && !std::memcmp(bytes.data(), "EFI PART", 8),
           "PIO reads continue after recovery");
}

int main()
{
    testStartup();
    testCommands();
    testReads();
    testWarmCard();
    testColdCard();
    testMultiBlockAndWrites();
    testUpgrade();
    testDmaReads();
    std::printf("PollingHost: %u checks, %u failures (simulated MMIO, not hardware)\n", checks, failures);
    return failures ? 1 : 0;
}
