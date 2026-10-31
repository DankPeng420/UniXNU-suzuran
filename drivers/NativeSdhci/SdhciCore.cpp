// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdhciCore.h"
#include "SdhciRegs.h"

namespace sdhci {

bool decodeCsd(const u32 csd[4], CsdInfo *out)
{
    if (!csd || !out) return false;

    const u32 structure   = csd[0] >> 30;
    const u32 readBlockLen = (csd[1] >> 16) & 0xF;
    const u32 cSize        = ((csd[1] & 0x3FF) << 2) | (csd[2] >> 30);
    const u32 cSizeMult    = (csd[2] >> 15) & 0x7;

    if (!structure || readBlockLen < 9 || readBlockLen > 11) return false;

    CsdInfo info = {};
    info.structure    = structure;
    info.mmcVersion   = (csd[0] >> 26) & 0xf;
    info.readBlockLen = readBlockLen;
    info.cSize        = cSize;
    info.cSizeMult    = cSizeMult;

    info.saturated = info.mmcVersion >= 4 && cSize == 0xfff;
    const u64 blocks = static_cast<u64>(cSize) + 1;
    const u64 mult = static_cast<u64>(1) << (cSizeMult + 2);
    const u64 length = static_cast<u64>(1) << readBlockLen;
    info.capacityBytes = info.saturated ? 0 : blocks * mult * length;

    *out = info;
    return true;
}

void normalizeResponse136(const u32 registers[4], u32 response[4])
{
    if (!registers || !response) return;
    u32 normalized[4];
    for (u32 word = 0; word < 4; ++word) {
        normalized[word] = registers[3 - word] << 8;
        if (word != 3) normalized[word] |= registers[2 - word] >> 24;
    }
    for (u32 word = 0; word < 4; ++word) response[word] = normalized[word];
}

bool decodeExtCsd(const u8 *bytes, u32 length, ExtCsdInfo *out)
{
    if (!bytes || length != 512 || !out || bytes[194] > 2 || bytes[61] > 1) return false;
    ExtCsdInfo info = {};
    info.capacityBytes = parseExtCsdSectorCount(bytes);
    info.sectorBytes = bytes[61] ? 4096 : 512;
    info.revision = bytes[192];
    info.partitionAccess = bytes[179] & 7;
    info.busWidth = bytes[183];
    info.hsTiming = bytes[185];
    info.deviceType = bytes[196];
    info.commandQueueEnabled = (bytes[15] & 1) != 0;
    *out = info;
    return true;
}

u64 parseExtCsdSectorCount(const u8 extCsd[512])
{
    if (!extCsd) return 0;
    const u32 sectors = static_cast<u32>(extCsd[212]) |
                             (static_cast<u32>(extCsd[213]) << 8) |
                             (static_cast<u32>(extCsd[214]) << 16) |
                             (static_cast<u32>(extCsd[215]) << 24);
    return static_cast<u64>(sectors) * 512;
}

u32 clockDivider(u32 baseHz, u32 targetHz)
{
    if (!baseHz || !targetHz) return 0;

    // The card clock is base / (2 * divider), so round the divider up to keep
    // the result at or below the target.
    const u64 denominator = static_cast<u64>(targetHz) * 2;
    u64 divider = (static_cast<u64>(baseHz) + denominator - 1) / denominator;
    if (divider < 1) divider = 1;
    if (divider > 1023) return 0;
    return static_cast<u32>(divider);
}

u16 encodeDivider(u32 divider)
{
    divider &= 0x3FF;
    return static_cast<u16>((((divider >> 8) & 0x3) << 6) | ((divider & 0xFF) << 8));
}

u32 decodeDivider(u16 clockControl)
{
    return ((static_cast<u32>(clockControl) >> 6) & 0x3) << 8 |
           ((static_cast<u32>(clockControl) >> 8) & 0xFF);
}

u32 buildAdma2(const Adma2Chunk *chunks, u32 count, u64 *table, u32 tableEntries)
{
    if (!chunks || !count || !table || count > tableEntries) return 0;

    for (u32 index = 0; index < count; ++index) {
        const Adma2Chunk &chunk = chunks[index];
        if (!chunk.length || chunk.length > 0x10000 || (chunk.length & 3)) return 0;
        if (chunk.phys & 3) return 0;
        // ADMA2 descriptors carry a 32-bit address; the Lumia eMMC controller
        // has no SMMU context, so every buffer must be reachable below 4 GiB.
        if (chunk.phys > 0xFFFFFFFFULL) return 0;

        u16 attributes = kAdma2AttrValid | kAdma2AttrActTransfer;
        if (index + 1 == count) attributes |= kAdma2AttrEnd;

        // A length of 65536 is encoded as zero, which the controller reads as
        // the full 64 KiB.
        const u64 length = chunk.length & 0xFFFF;
        table[index] = static_cast<u64>(attributes) | (length << 16) | (chunk.phys << 32);
    }
    return count;
}

u64 maxValidLba(u64 capacityBytes)
{
    return capacityBytes >= 512 ? capacityBytes / 512 - 1 : 0;
}

u32 crc32(const u8 *bytes, u32 length)
{
    u32 crc = 0xffffffffu;
    for (u32 index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (u32 bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

namespace {
u32 le32(const u8 *at)
{
    return static_cast<u32>(at[0]) | (static_cast<u32>(at[1]) << 8) |
           (static_cast<u32>(at[2]) << 16) | (static_cast<u32>(at[3]) << 24);
}

u64 le64(const u8 *at)
{
    return static_cast<u64>(le32(at)) | (static_cast<u64>(le32(at + 4)) << 32);
}

bool entryUsed(const u8 *entry)
{
    u8 used = 0;
    for (u32 index = 0; index < 16; ++index) used |= entry[index];
    return used != 0;
}

bool entryNamed(const u8 *entry, const char *name)
{
    u32 index = 0;
    for (; name[index]; ++index) {
        if (index >= 36 || entry[56 + 2 * index] != static_cast<u8>(name[index]) || entry[57 + 2 * index]) return false;
    }
    return index == 36 || (entry[56 + 2 * index] == 0 && entry[57 + 2 * index] == 0);
}
}

bool findGptPartition(const u8 *header, const u8 *entries, u32 entriesBytes, u64 lastLba,
                      const char *name, GptRange *out)
{
    if (!header || !entries || !name || !name[0] || !out) return false;
    if (le64(header) != 0x5452415020494645ULL || le32(header + 8) != 0x00010000) return false;
    const u32 headerBytes = le32(header + 12);
    if (headerBytes < 92 || headerBytes > 512 || le32(header + 20) || le64(header + 24) != 1) return false;
    u8 copy[512];
    for (u32 index = 0; index < headerBytes; ++index) copy[index] = header[index];
    for (u32 index = 16; index < 20; ++index) copy[index] = 0;
    if (crc32(copy, headerBytes) != le32(header + 16)) return false;
    const u64 firstUsable = le64(header + 40), lastUsable = le64(header + 48);
    const u32 count = le32(header + 80), entryBytes = le32(header + 84);
    if (entryBytes != 128 || !count || count > 256 || count * entryBytes > entriesBytes ||
        le64(header + 72) < 2 || lastUsable < firstUsable || lastUsable > lastLba) return false;
    if (crc32(entries, count * entryBytes) != le32(header + 88)) return false;
    bool found = false;
    GptRange range = {};
    for (u32 index = 0; index < count; ++index) {
        const u8 *entry = entries + index * entryBytes;
        if (!entryUsed(entry) || !entryNamed(entry, name)) continue;
        const u64 first = le64(entry + 32), last = le64(entry + 40);
        if (found || first < firstUsable || last < first || last > lastUsable) return false;
        range = {first, last};
        found = true;
    }
    if (!found) return false;
    *out = range;
    return true;
}

} // namespace sdhci
