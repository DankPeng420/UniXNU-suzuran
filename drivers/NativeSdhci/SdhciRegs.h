// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// SDHCI host controller registers, as used by the Lumia 950 (MSM8992) eMMC.
//
// Offsets follow the SDHCI specification (Host Controller 3.0 layout, which is
// what Linux's drivers/mmc/host/sdhci.h uses) and are relative to the "hc"
// window at 0xf9824900 on this board. Note that the interrupt registers are
// 32-bit words at 0x30/0x34/0x38: the low half is the normal status and the
// high half the error status.
//
// Only the standard register set is defined. The Qualcomm QSDCC vendor window
// ("core" in the device tree) holds the DLL and power-control registers needed
// for HS200/HS400; those offsets are deliberately absent until they are
// confirmed against the firmware that programs them. See README.md, "Vendor
// registers".
#ifndef NATIVE_SDHCI_REGS_H
#define NATIVE_SDHCI_REGS_H

#include "SdhciTypes.h"

namespace sdhci {

// Register offsets, relative to the "hc" window.
enum : u32 {
    kRegDmaAddress       = 0x00,
    kRegBlockSize        = 0x04,
    kRegBlockCount       = 0x06,
    kRegArgument         = 0x08,
    kRegTransferMode     = 0x0C,
    kRegCommand          = 0x0E,
    kRegResponse         = 0x10,   // 0x10..0x1F
    kRegBufferPort       = 0x20,
    kRegPresentState     = 0x24,
    kRegHostControl1     = 0x28,
    kRegPowerControl     = 0x29,
    kRegBlockGapControl  = 0x2A,
    kRegWakeUpControl    = 0x2B,
    kRegClockControl     = 0x2C,
    kRegTimeoutControl   = 0x2E,
    kRegSoftwareReset    = 0x2F,
    kRegInterruptStatus  = 0x30,   // 32-bit: normal in the low half, error in the high half
    kRegInterruptEnable  = 0x34,
    kRegInterruptSignal  = 0x38,
    kRegAutoCmd12Error   = 0x3C,
    kRegHostControl2     = 0x3E,
    kRegCapabilities     = 0x40,
    kRegCapabilities1    = 0x44,
    kRegMaxCurrent       = 0x48,
    kRegAdmaErrorStatus  = 0x54,
    kRegAdmaAddress      = 0x58,   // 64-bit address in 0x58 and 0x5C
    kRegPresetValue      = 0x60,   // 0x60..0x7F
    kRegSlotIntStatus    = 0xFC,
    kRegHostVersion      = 0xFE,
};

// Present State (0x24, 32-bit)
enum : u32 {
    kPresentCmdInhibit    = 0x00000001,
    kPresentDataInhibit   = 0x00000002,
    kPresentDatLineActive = 0x00000004,
    kPresentWriteTransfer = 0x00000100,
    kPresentReadTransfer  = 0x00000200,
    kPresentBufferWrite   = 0x00000400,   // space available for the host to write
    kPresentBufferRead    = 0x00000800,   // data available for the host to read
    kPresentCardInserted  = 0x00010000,
};

// Command (0x0E, 16-bit)
enum : u16 {
    kCmdResponseNone      = 0x00,
    kCmdResponse136       = 0x01,
    kCmdResponse48        = 0x02,
    kCmdResponse48Busy    = 0x03,
    kCmdCrcCheck          = 0x08,
    kCmdIndexCheck        = 0x10,
    kCmdDataPresent       = 0x20,
};

// Transfer Mode (0x0C, 16-bit)
enum : u16 {
    kModeDmaEnable        = 0x01,
    kModeBlockCountEnable = 0x02,
    kModeAutoCmd12        = 0x04,
    kModeAutoCmd23        = 0x08,
    kModeDirectionRead    = 0x10,
    kModeMultiBlock       = 0x20,
};

// Host Control 1 (0x28, 8-bit)
enum : u8 {
    kHostLedPower         = 0x01,
    kHostDataWidth4       = 0x02,
    kHostHighSpeed        = 0x04,
    kHostDmaSelectMask    = 0x18,
    kHostDmaSelectSdma    = 0x00,
    kHostDmaSelectAdma1   = 0x08,
    kHostDmaSelectAdma2   = 0x10,
    kHostDmaSelectAdma64  = 0x18,
    kHostDataWidth8       = 0x20,
};

// Power Control (0x29, 8-bit)
enum : u8 {
    kPowerBusOn           = 0x01,
    kPowerVoltage180      = 0x0A,
    kPowerVoltage300      = 0x0C,
    kPowerVoltage330      = 0x0E,
};

// Clock Control (0x2C, 16-bit): the divider's low eight bits sit at bits 15..8
// and its high two bits at bits 7..6.
enum : u16 {
    kClockInternalEnable  = 0x0001,
    kClockInternalStable  = 0x0002,
    kClockSdClockEnable   = 0x0004,
};

// Software Reset (0x2F, 8-bit)
enum : u8 {
    kResetAll             = 0x01,
    kResetCommand         = 0x02,
    kResetData            = 0x04,
};

// Interrupt Status (0x30, 32-bit)
enum : u32 {
    kIntCommandComplete   = 0x00000001,
    kIntTransferComplete  = 0x00000002,
    kIntBlockGap          = 0x00000004,
    kIntDma               = 0x00000008,
    kIntBufferWriteReady  = 0x00000010,
    kIntBufferReadReady   = 0x00000020,
    kIntCardInsertion     = 0x00000040,
    kIntCardRemoval       = 0x00000080,
    kIntCardInterrupt     = 0x00000100,
    kIntError             = 0x00008000,
    kErrCommandTimeout    = 0x00010000,
    kErrCommandCrc        = 0x00020000,
    kErrCommandEndBit     = 0x00040000,
    kErrCommandIndex      = 0x00080000,
    kErrDataTimeout       = 0x00100000,
    kErrDataCrc           = 0x00200000,
    kErrDataEndBit        = 0x00400000,
    kErrCurrentLimit      = 0x00800000,
    kErrAutoCmd12         = 0x01000000,
    kErrAdma              = 0x02000000,
};

// Capabilities (0x40, 32-bit)
enum : u32 {
    kCapTimeoutClockMask  = 0x0000003F,
    kCapBaseClockShift    = 8,
    kCapBaseClockMask     = 0x000000FF,
};

// ADMA2 descriptor attributes (low 16 bits of the first descriptor word).
enum : u16 {
    kAdma2AttrValid       = 1u << 0,
    kAdma2AttrEnd         = 1u << 1,
    kAdma2AttrInt         = 1u << 2,
    kAdma2AttrActNop      = 0u << 4,
    kAdma2AttrActReserved = 1u << 4,
    kAdma2AttrActTransfer = 2u << 4,
    kAdma2AttrActLink     = 3u << 4,
};

// eMMC commands used by the bring-up path.
enum : u8 {
    kCmd0GoIdleState       = 0,
    kCmd1SendOpCond        = 1,
    kCmd2AllSendCid        = 2,
    kCmd3SetRelativeAddr   = 3,
    kCmd6Switch            = 6,
    kCmd7SelectCard        = 7,
    kCmd8SendExtCsd        = 8,
    kCmd9SendCsd           = 9,
    kCmd12StopTransmission = 12,
    kCmd13SendStatus       = 13,
    kCmd16SetBlockLength   = 16,
    kCmd17ReadSingleBlock  = 17,
    kCmd18ReadMultiple     = 18,
    kCmd24WriteSingleBlock = 24,
    kCmd25WriteMultiple    = 25,
};

} // namespace sdhci

#endif // NATIVE_SDHCI_REGS_H
