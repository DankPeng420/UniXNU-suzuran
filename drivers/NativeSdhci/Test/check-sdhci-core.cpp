// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Host test for SdhciCore. Build and run on the development machine:
//
//   g++ -std=c++17 -Wall -Wextra -Werror -I drivers/NativeSdhci
//       drivers/NativeSdhci/Test/check-sdhci-core.cpp
//       drivers/NativeSdhci/SdhciCore.cpp -o build/check-sdhci-core
//   ./build/check-sdhci-core
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../SdhciCore.h"
#include "../SdhciRegs.h"

using sdhci::u8;
using sdhci::u16;
using sdhci::u32;
using sdhci::u64;

static int failures = 0;

static void expect(bool condition, const char *what)
{
    if (condition) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static void expectEq(u64 actual, u64 expected, const char *what)
{
    if (actual == expected) {
        std::printf("ok   %s (%llu)\n", what, static_cast<unsigned long long>(actual));
    } else {
        std::printf("FAIL %s: got %llu, expected %llu\n", what,
                    static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
        ++failures;
    }
}

// A CSD v1.0 image describing exactly 1 GiB.
// C_SIZE [73:62] = 4095 spans csd[1] bits 9..0 and csd[2] bits 31..30,
// C_SIZE_MULT [49:47] = 7, READ_BL_LEN [83:80] = 9 (512 bytes).
static const u32 kCsd1GiB[4] = { 0x4c000000u, 0x000903FFu, 0xC0038000u, 0x00000000u };

// A saturated eMMC CSD v3.0 image: structure 2, C_SIZE at its 12-bit maximum.
static const u32 kCsdSaturated[4] = { 0xd0000000u, 0x000903FFu, 0xC0038000u, 0x00000000u };

int main()
{
    // --- clock divider ---------------------------------------------------
    expectEq(sdhci::clockDivider(200000000u, 100000000u), 1, "200 MHz base reaches 100 MHz with divider 1");
    expectEq(sdhci::clockDivider(200000000u, 52000000u), 2, "200 MHz base rounds up for 52 MHz");
    expectEq(sdhci::clockDivider(200000000u, 400000u), 250, "200 MHz base reaches 400 kHz");
    expectEq(sdhci::clockDivider(1000000u, 100000000u), 1, "an unreachable target clamps to the fastest clock");
    expectEq(sdhci::clockDivider(0, 1000000u), 0, "no base clock is a failure");
    expectEq(sdhci::clockDivider(4000000000u, 1u), 0, "an out-of-range divider is refused");

    for (u32 divider : { 1u, 2u, 255u, 256u, 257u, 1023u }) {
        char what[64];
        std::snprintf(what, sizeof what, "divider %u survives encode/decode", divider);
        expectEq(sdhci::decodeDivider(sdhci::encodeDivider(divider)), divider, what);
    }
    expectEq(sdhci::encodeDivider(1023) & 0xFFC0, 0xFFC0, "1023 sets both divider fields");

    // --- CSD -------------------------------------------------------------
    sdhci::CsdInfo info = {};
    expect(sdhci::decodeCsd(kCsd1GiB, &info), "1 GiB CSD decodes");
    expectEq(info.capacityBytes, 1ULL << 30, "1 GiB capacity");
    expectEq(info.readBlockLen, 9, "read block length 512");
    expect(!info.saturated, "a 1 GiB CSD is not saturated");

    expect(sdhci::decodeCsd(kCsdSaturated, &info), "saturated eMMC CSD decodes");
    expect(info.saturated, "a 12-bit C_SIZE maximum is reported as saturated");
    expectEq(info.capacityBytes, 0, "a saturated CSD reports no capacity");
    expectEq(info.structure, 3, "modern eMMC CSD structure 3 is valid");
    expectEq(info.mmcVersion, 4, "MMC specification version is decoded");

    u32 bad[4] = { 0, 0x00090000, 0, 0 };
    expect(!sdhci::decodeCsd(bad, &info), "unsupported MMC structure zero is refused");
    u32 badLen[4] = { 0, 0x000F0000u, 0, 0 };  // READ_BL_LEN = 15
    expect(!sdhci::decodeCsd(badLen, &info), "an impossible block length is refused");
    expect(!sdhci::decodeCsd(nullptr, &info), "a null CSD is refused");
    const u32 csdStructure2[4] = { 0x90000000, 0x00090000, 0, 0 };
    expect(sdhci::decodeCsd(csdStructure2, &info), "structure 2 MMC CSD decodes");
    expectEq(info.capacityBytes, 2048, "MMC capacity uses C_SIZE_MULT rather than an SDHC formula");
    const u32 rawResponse[4] = { 0x01234567, 0x89abcdef, 0x13579bdf, 0x2468ace0 };
    u32 normalized[4] = {};
    sdhci::normalizeResponse136(rawResponse, normalized);
    expectEq(normalized[0], 0x68ace013, "R2 top word reverses response registers and restores CRC shift");
    expectEq(normalized[1], 0x579bdf89, "R2 middle word carries the adjacent byte");
    expectEq(normalized[2], 0xabcdef01, "R2 second-low word carries the adjacent byte");
    expectEq(normalized[3], 0x23456700, "R2 low byte is the omitted CRC byte");
    sdhci::normalizeResponse136(normalized, normalized);
    expectEq(normalized[0], 0x456700ab, "in-place R2 normalization has no overwrite hazard");

    // --- EXT_CSD ---------------------------------------------------------
    u8 extCsd[512];
    std::memset(extCsd, 0, sizeof extCsd);
    const u32 sectors = 0x04000000u;   // 32 GiB in 512-byte units
    extCsd[212] = sectors & 0xFF;
    extCsd[213] = (sectors >> 8) & 0xFF;
    extCsd[214] = (sectors >> 16) & 0xFF;
    extCsd[215] = (sectors >> 24) & 0xFF;
    expectEq(sdhci::parseExtCsdSectorCount(extCsd), 32ULL << 30, "EXT_CSD SEC_COUNT gives 32 GiB");
    std::memset(extCsd, 0xFF, sizeof extCsd);
    expectEq(sdhci::parseExtCsdSectorCount(extCsd), 0xFFFFFFFFULL * 512ULL, "EXT_CSD is little-endian");

    // --- ADMA2 -----------------------------------------------------------
    sdhci::Adma2Chunk chunks[2] = { { 0x80000000ULL, 4096 }, { 0x80001000ULL, 8192 } };
    u64 table[4] = {};
    expectEq(sdhci::buildAdma2(chunks, 2, table, 4), 2, "two chunks build two descriptors");
    expectEq(table[0] & 0xFFFF, sdhci::kAdma2AttrValid | sdhci::kAdma2AttrActTransfer, "first descriptor is a valid transfer");
    expectEq((table[0] >> 16) & 0xFFFF, 4096, "first descriptor carries its length");
    expectEq(table[0] >> 32, 0x80000000ULL, "first descriptor carries its address");
    expectEq(table[1] & 0xFFFF, sdhci::kAdma2AttrValid | sdhci::kAdma2AttrActTransfer | sdhci::kAdma2AttrEnd,
             "last descriptor ends the table");
    expectEq(sdhci::buildAdma2(chunks, 2, table, 1), 0, "a short table is refused");

    sdhci::Adma2Chunk unaligned[1] = { { 0x80000003ULL, 4096 } };
    expectEq(sdhci::buildAdma2(unaligned, 1, table, 4), 0, "an unaligned address is refused");
    sdhci::Adma2Chunk oddLength[1] = { { 0x80000000ULL, 4095 } };
    expectEq(sdhci::buildAdma2(oddLength, 1, table, 4), 0, "a length that is not a multiple of 4 is refused");
    sdhci::Adma2Chunk above4G[1] = { { 0x100000000ULL, 4096 } };
    expectEq(sdhci::buildAdma2(above4G, 1, table, 4), 0, "a 64-bit address is refused");
    sdhci::Adma2Chunk full[1] = { { 0x80000000ULL, 0x10000 } };
    expectEq(sdhci::buildAdma2(full, 1, table, 4), 1, "a 64 KiB chunk is accepted");
    expectEq((table[0] >> 16) & 0xFFFF, 0, "64 KiB is encoded as a zero length field");

    // --- capacity --------------------------------------------------------
    expectEq(sdhci::maxValidLba(32ULL << 30), (32ULL << 21) - 1, "last LBA of 32 GiB");
    expectEq(sdhci::maxValidLba(0), 0, "an empty medium has no valid LBA");

    // --- GPT ---------------------------------------------------------------
    expectEq(sdhci::crc32(reinterpret_cast<const u8 *>("123456789"), 9), 0xcbf43926u, "CRC-32 check value");
    {
        const u64 lastLba = 61071359;
        static u8 header[512];
        static u8 entries[128 * 128];
        auto put32 = [](u8 *at, u32 value) { for (u32 i = 0; i < 4; ++i) at[i] = static_cast<u8>(value >> (8 * i)); };
        auto put64 = [&](u8 *at, u64 value) { put32(at, static_cast<u32>(value)); put32(at + 4, static_cast<u32>(value >> 32)); };
        auto name = [](u8 *entry, const char *text) {
            for (u32 i = 0; i < 72; ++i) entry[56 + i] = 0;
            for (u32 i = 0; text[i]; ++i) entry[56 + 2 * i] = static_cast<u8>(text[i]);
        };
        auto partition = [&](u32 index, const char *text, u64 first, u64 last) {
            u8 *entry = entries + index * 128;
            entry[0] = 0xaf;                       // any non-zero type GUID marks the entry used
            entry[16] = static_cast<u8>(index + 1);
            put64(entry + 32, first);
            put64(entry + 40, last);
            name(entry, text);
        };
        auto seal = [&]() {
            put32(header + 88, sdhci::crc32(entries, sizeof entries));
            put32(header + 16, 0);
            put32(header + 16, sdhci::crc32(header, 92));
        };
        std::memcpy(header, "EFI PART", 8);
        put32(header + 8, 0x00010000);
        put32(header + 12, 92);
        put64(header + 24, 1);
        put64(header + 32, lastLba);
        put64(header + 40, 34);
        put64(header + 48, lastLba - 33);
        put64(header + 72, 2);
        put32(header + 80, 128);
        put32(header + 84, 128);
        partition(0, "userdata", 8323072, 25100287);
        partition(1, "ios", 25100288, 61069311);
        partition(2, "iosx", 100, 200);
        seal();
        sdhci::GptRange range = {};
        expect(sdhci::findGptPartition(header, entries, sizeof entries, lastLba, "ios", &range), "the ios partition is found");
        expectEq(range.first, 25100288, "ios first LBA");
        expectEq(range.last, 61069311, "ios last LBA");
        expect(!sdhci::findGptPartition(header, entries, sizeof entries, lastLba, "io", &range), "a name prefix does not match");
        expect(!sdhci::findGptPartition(header, entries, 1024, lastLba, "ios", &range), "a short entry array is refused");
        expect(!sdhci::findGptPartition(header, entries, sizeof entries, 1000, "ios", &range), "a usable range past the medium is refused");
        entries[200] ^= 1;
        expect(!sdhci::findGptPartition(header, entries, sizeof entries, lastLba, "ios", &range), "an entry CRC mismatch is refused");
        entries[200] ^= 1;
        header[60] ^= 1;
        expect(!sdhci::findGptPartition(header, entries, sizeof entries, lastLba, "ios", &range), "a header CRC mismatch is refused");
        header[60] ^= 1;
        partition(3, "ios", 300, 400);
        seal();
        expect(!sdhci::findGptPartition(header, entries, sizeof entries, lastLba, "ios", &range), "a duplicated name is refused");
        partition(3, "other", 300, 400);
        partition(1, "ios", 25100288, lastLba);
        seal();
        expect(!sdhci::findGptPartition(header, entries, sizeof entries, lastLba, "ios", &range), "a partition past the usable range is refused");
    }

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
