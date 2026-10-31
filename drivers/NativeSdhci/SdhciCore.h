// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Host-testable part of the Lumia 950 eMMC driver: the arithmetic and the
// table decoding. Nothing here touches hardware, IOKit or XNU, so it compiles
// and runs on the build host (drivers/NativeSdhci/Test/check-sdhci-core.cpp).
#ifndef NATIVE_SDHCI_CORE_H
#define NATIVE_SDHCI_CORE_H

#include "SdhciTypes.h"

namespace sdhci {

// Decoded CSD. eMMC parts above 2 GiB report a saturated CSD, so capacity for
// those comes from EXT_CSD (parseExtCsdSectorCount) instead.
struct CsdInfo {
    u32 structure;
    u32 mmcVersion;
    u32 readBlockLen;    // log2 of the block length
    u32 cSize;
    u32 cSizeMult;
    u64 capacityBytes;   // from the CSD alone; 0 when it cannot express it
    bool saturated;
};

// Decodes a 128-bit CSD as four big-endian words, csd[0] holding bits 127..96.
bool decodeCsd(const u32 csd[4], CsdInfo *out);

void normalizeResponse136(const u32 registers[4], u32 response[4]);

struct ExtCsdInfo {
    u64 capacityBytes;
    u32 sectorBytes;
    u8 revision;
    u8 partitionAccess;
    u8 busWidth;         // BUS_WIDTH [183]: write-only, reads back 0 on real cards
    u8 hsTiming;         // HS_TIMING [185]: 0 legacy, 1 high speed
    u8 deviceType;       // DEVICE_TYPE [196]: bit 1 = high speed at 52 MHz
    bool commandQueueEnabled;
};

bool decodeExtCsd(const u8 *bytes, u32 length, ExtCsdInfo *out);

// Reads SEC_COUNT (offset 212, little-endian, 512-byte units) from a 512-byte
// EXT_CSD image. Returns 0 when the image cannot describe a usable capacity.
u64 parseExtCsdSectorCount(const u8 extCsd[512]);

// SDHCI 3.0 clock arithmetic: the card clock is base / (2 * divider), with a
// 10-bit divider and 1 as its minimum. Returns 0 when targetHz is unreachable.
u32 clockDivider(u32 baseHz, u32 targetHz);

// Encodes a 10-bit divider into Clock Control bits [7:6] (high two bits) and
// [15:8] (low eight bits), and decodes it back.
u16 encodeDivider(u32 divider);
u32 decodeDivider(u16 clockControl);

// One ADMA2 transfer chunk: a physical address and a length of at most 64 KiB
// in a multiple of 4 bytes.
struct Adma2Chunk {
    u64 phys;
    u32 length;
};

// Writes an ADMA2 descriptor table for the chunks and marks the last descriptor
// as the end. Returns the descriptor count, or 0 on a malformed chunk or when
// the table is too small. Descriptors are 8 bytes each in the host's byte order.
u32 buildAdma2(const Adma2Chunk *chunks, u32 count, u64 *table, u32 tableEntries);

// Last valid 512-byte LBA of a medium, or 0 for an empty one.
u64 maxValidLba(u64 capacityBytes);

// The reflected IEEE CRC-32 GPT uses.
u32 crc32(const u8 *bytes, u32 length);

// A partition's inclusive 512-byte LBA range.
struct GptRange {
    u64 first;
    u64 last;
};

// Finds the one partition named `name` (ASCII, compared with the UTF-16LE GPT name) in a primary GPT:
// `header` is LBA 1 (512 bytes) and `entries` the partition array it points to, `entriesBytes` long.
// Both CRCs, the header revision and size, the entry geometry and the partition bounds against the
// usable range and `lastLba` of the medium are checked; a missing or duplicated name is refused.
bool findGptPartition(const u8 *header, const u8 *entries, u32 entriesBytes, u64 lastLba,
                      const char *name, GptRange *out);

} // namespace sdhci

#endif // NATIVE_SDHCI_CORE_H
