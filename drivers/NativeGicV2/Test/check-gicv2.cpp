#include "../GicV2Core.h"
#include <array>
#include <cstdio>
#include <vector>

using namespace gicv2;

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

enum class View { Single, Nonsecure, Secure };
struct Access {Space space; u32 offset; u32 value;};

struct FakeGic {
    std::array<u32, 1024> dist = {};
    std::array<u32, 1024> cpu = {};
    std::vector<Access> writes;
    View view;
    u32 blocked = 0xffffffff;
    unsigned barriers = 0;
    unsigned invalidAccesses = 0;
    bool rejectEnable = false;
    bool rejectPriorityMask = false;
    u32 priorityBits = 0xff;
    u32 constantPriorityMask = 0x100;

    explicit FakeGic(View mode = View::Nonsecure, u8 target = 0x20) : view(mode)
    {
        dist[0xfe8 / 4] = 0x20;
        dist[4 / 4] = 31 | (7u << 5) | (mode == View::Single ? 0 : 1u << 10);
        for (u32 offset = 0x800; offset < 0x820; offset += 4) dist[offset / 4] = 0x01010101u * target;
        for (u32 offset = 0x80; offset < 0x100; offset += 4) dist[offset / 4] = 0xffffffffu;
        cpu[0] = 0x600;
        cpu[0xc / 4] = 1023;
    }

    static u32 read(void *context, Space space, u32 offset)
    {
        auto &fake = *static_cast<FakeGic *>(context);
        if (offset >= 4096 || (offset & 3)) {
            ++fake.invalidAccesses;
            return 0xffffffff;
        }
        if (space == Space::Cpu) {
            if (offset == 4 && fake.rejectPriorityMask) return 0;
            if (offset == 4 && fake.constantPriorityMask <= 0xff) return fake.constantPriorityMask;
            return fake.cpu[offset / 4];
        }
        if (offset >= 0x80 && offset < 0x100 && fake.view != View::Secure) return 0;
        if (offset >= 0x400 && offset < 0x800 && fake.blocked != 0xffffffff) {
            u32 value = fake.dist[offset / 4];
            if ((fake.blocked & ~3u) == offset - 0x400) value &= ~(0xffu << ((fake.blocked & 3) * 8));
            return value;
        }
        return fake.dist[offset / 4];
    }

    static void write(void *context, Space space, u32 offset, u32 value)
    {
        auto &fake = *static_cast<FakeGic *>(context);
        fake.writes.push_back({space, offset, value});
        if (offset >= 4096 || (offset & 3)) {
            ++fake.invalidAccesses;
            return;
        }
        if (space == Space::Cpu) {
            fake.cpu[offset / 4] = offset == 4 ? value & fake.priorityBits : value;
            return;
        }
        if (offset >= 0x80 && offset < 0x100 && fake.view != View::Secure) return;
        if (offset >= 0x800 && offset < 0x820) return;
        if (offset >= 0x100 && offset < 0x180) {
            if (!fake.rejectEnable) fake.dist[offset / 4] |= value;
        } else if (offset >= 0x180 && offset < 0x200) fake.dist[(offset - 0x80) / 4] &= ~value;
        else if (offset >= 0x280 && offset < 0x300) fake.dist[(offset - 0x80) / 4] &= ~value;
        else if (offset >= 0x380 && offset < 0x400) fake.dist[(offset - 0x80) / 4] &= ~value;
        else fake.dist[offset / 4] = offset >= 0x400 && offset < 0x800 ?
            value & (fake.priorityBits * 0x01010101u) : value;
    }

    static void barrier(void *context)
    {
        ++static_cast<FakeGic *>(context)->barriers;
    }

    RegisterIo io() {return {this, read, write, barrier};}
};

void prepared(FakeGic &fake, Controller &controller)
{
    expect(controller.configure(fake.io(), 4096, 4096), "GICv2 windows and PIDR2 configure");
    expect(controller.prepareDistributor(), "distributor prepares");
    expect(controller.prepareLocal(), "banked SGI and PPI registers prepare");
}

void testCompatibility()
{
    const char lumia[] = "qcom,msm-qgic2\0arm,gic-2";
    const char generic[] = "arm,cortex-a15-gic";
    const char v3[] = "vendor,controller\0arm,gic-v3";
    const char conflict[] = "arm,gic-2\0arm,gic-v3";
    const char malformed[] = {'a', 'r', 'm', ',', 'g', 'i', 'c', '-', '2'};
    expect(compatibleVersion(lumia, sizeof lumia) == 2, "Lumia compatible string list is recognized");
    expect(compatibleVersion(generic, sizeof generic) == 2, "QEMU cortex-a15 GIC is recognized");
    expect(compatibleVersion("arm,gic-400", 12) == 2, "GIC-400 compatible is recognized");
    expect(compatibleVersion(v3, sizeof v3) == 3, "GICv3 compatible can appear after vendor entry");
    expect(compatibleVersion(conflict, sizeof conflict) == 0, "conflicting controller generations are refused");
    expect(compatibleVersion(malformed, sizeof malformed) == 0, "unterminated compatible property is refused");
    expect(compatibleVersion(nullptr, 1) == 0, "null compatible property is refused");
    expect(compatibleVersion(lumia, sizeof lumia - 1) == 0, "matching prefix cannot hide malformed trailing string");
    expect(interruptId(0x1400 | 27) == 27, "GICv2 source-CPU tag is not part of INTID");
}

void testViews()
{
    for (View view : {View::Single, View::Nonsecure, View::Secure}) {
        FakeGic fake(view);
        Controller controller;
        prepared(fake, controller);
        expect(controller.secureView() == (view == View::Secure), "security extension register view is detected");
        expect(controller.cpuTarget() == 0x20, "CPU target comes from banked ITARGETSR, not MPIDR/logical ID");
        expect(controller.interruptCount() == 1020, "special INTIDs 1020..1023 are not routable interrupts");
        expect(controller.enableLocal(), "GICC memory-mapped interface enables");
        expect((fake.cpu[0] & 0x601) == 1, "both EOI split modes are cleared");
        expect(fake.cpu[4 / 4] == 0xff && fake.cpu[8 / 4] == 0, "priority mask and binary point are initialized");
        expect((fake.dist[0x100 / 4] & (1u | (1u << 27))) == (1u | (1u << 27)), "SGI 0 and virtual timer PPI 27 are enabled");
        if (view == View::Secure) {
            expect((fake.dist[0x80 / 4] & (1u | (1u << 27))) == 0, "secure-view owned sources use Group 0 IRQs");
        }
        expect(controller.sendSgi(controller.cpuTarget()), "self IPI uses the discovered target mask");
        expect(fake.dist[0xf00 / 4] == 0x00200000, "SGIR packs target bits in 23:16 and SGI 0 in 3:0");
        const u32 raw = (5u << 10) | 0;
        fake.cpu[0xc / 4] = raw;
        expect(controller.acknowledge() == raw, "full raw IAR token is retained");
        expect(controller.end(raw), "valid interrupt is ended");
        expect(fake.cpu[0x10 / 4] == raw, "EOIR receives source CPU bits, not just INTID");
        for (u32 special = 1020; special <= 1023; ++special) {
            const auto writes = fake.writes.size();
            expect(!controller.end(special), "special/spurious token is not ended");
            expect(fake.writes.size() == writes, "spurious acknowledgement emits no EOIR");
        }
        controller.disableLocal();
        expect((fake.cpu[0] & 3) == 0, "quiesce disables GICC without GICv3 system registers");
        expect(fake.invalidAccesses == 0, "no GICv3 frame or out-of-window access");
        expect(fake.barriers >= fake.writes.size(), "MMIO writes are ordered");
    }
}

void testSpi()
{
    FakeGic fake;
    Controller controller;
    prepared(fake, controller);
    expect(controller.enableLocal(), "SPI host enables local interface");
    for (u32 id : {32u, 40u, 155u, 1019u}) {
        const u32 config = 0xc00 + (id / 16) * 4;
        const u32 shift = (id % 16) * 2;
        const u32 target = 0x800 + (id & ~3u);
        const u32 targetShift = (id & 3) * 8;
        fake.dist[target / 4] = 0x01020408;
        const u32 untouched = fake.dist[target / 4] & ~(0xffu << targetShift);
        expect(controller.configureSpi(id, true), "edge SPI configures while masked");
        expect(!(fake.dist[(0x100 + (id / 32) * 4) / 4] & (1u << (id % 32))), "SPI remains masked until handler publication");
        expect(((fake.dist[config / 4] >> shift) & 3) == 2, "edge uses the correct ICFGR two-bit field");
        expect(((fake.dist[target / 4] >> targetShift) & 0xff) == 0x20, "SPI routes to the discovered boot interface");
        expect((fake.dist[target / 4] & ~(0xffu << targetShift)) == untouched, "neighboring SPI target bytes are preserved");
        expect(controller.enable(id), "published SPI can be unmasked");
        expect(controller.mask(id), "SPI can be masked");
        expect(controller.configureSpi(id, false), "SPI can be changed to level high");
        expect(((fake.dist[config / 4] >> shift) & 3) == 0, "level-high clears both field bits");
    }
    for (u32 bad : {0u, 27u, 1020u, 1023u, 0xffffffffu}) {
        const auto before = fake.writes.size();
        expect(!controller.configureSpi(bad, false), "invalid SPI INTID is refused");
        expect(fake.writes.size() == before, "invalid SPI never touches MMIO");
    }
    expect(!controller.sendSgi(0), "empty SGI target is refused");
    expect(!controller.sendSgi(1, 16), "SGI 16 is refused");
    expect(!controller.configureSpi(40, false, 0xff), "masked-by-PMR SPI priority is refused");
    fake.blocked = 41;
    expect(!controller.configureSpi(41, false), "secure-owned SPI rejects inaccessible priority writes");
    expect(controller.error() == Error::RegisterRejected, "ownership refusal is observable");
}

void testRefusals()
{
    FakeGic fake;
    Controller shortWindow;
    expect(!shortWindow.configure(fake.io(), 4095, 4096), "short distributor window is refused");
    expect(!shortWindow.configure(fake.io(), 4096, 4095), "short GICC window is refused");
    expect(fake.writes.empty(), "invalid windows cause no MMIO writes");
    RegisterIo invalid = fake.io();
    invalid.barrier = nullptr;
    expect(!shortWindow.configure(invalid, 4096, 4096), "missing ordering callback is refused");
    fake.dist[0xfe8 / 4] = 0x30;
    expect(!shortWindow.configure(fake.io(), 4096, 4096), "GICv3 PIDR2 is not treated as GICv2");
    expect(shortWindow.error() == Error::WrongArchitecture, "wrong architecture has a diagnostic");
    expect(!shortWindow.prepareDistributor(), "unbound controller cannot prepare");

    {
        FakeGic qgic;
        qgic.dist[0xfe8 / 4] = 0;
        Controller controller;
        expect(controller.configure(qgic.io(), 4096, 4096), "QGIC2 without PIDR2 configures from TYPER");
        expect(controller.error() == Error::None, "accepted QGIC2 leaves no diagnostic");
        FakeGic blank;
        blank.dist[0xfe8 / 4] = 0;
        blank.dist[4 / 4] = 0;
        Controller unmapped;
        expect(!unmapped.configure(blank.io(), 4096, 4096), "an all-zero window is not a GIC");
        expect(unmapped.error() == Error::WrongArchitecture, "all-zero window has a diagnostic");
    }

    for (u8 target : {static_cast<u8>(0), static_cast<u8>(3)}) {
        FakeGic badTarget(View::Single, target);
        Controller controller;
        expect(controller.configure(badTarget.io(), 4096, 4096), "target test controller configures");
        expect(!controller.prepareDistributor(), "zero or multi-interface boot target is refused");
        expect(controller.error() == Error::InvalidCpuTarget, "invalid target has a diagnostic");
    }
    FakeGic disabled;
    Controller controller;
    prepared(disabled, controller);
    disabled.rejectEnable = true;
    expect(!controller.enableLocal(), "inaccessible local enable is not called success");
    expect(controller.error() == Error::RegisterRejected, "failed enable is observable");
    FakeGic blockedMask;
    Controller blockedController;
    prepared(blockedMask, blockedController);
    blockedMask.rejectPriorityMask = true;
    expect(!blockedController.enableLocal(), "secure priority-mask ownership refusal is not success");
    Controller unprepared;
    expect(!unprepared.enableLocal(), "unprepared CPU interface cannot enable");
    expect(unprepared.acknowledge() == 1023, "unprepared controller returns spurious token without MMIO");
    expect(!unprepared.sendSgi(1), "unprepared controller cannot send SGIs");
}

void testPriorityPrecision()
{
    for (u32 bits : {0xf0u, 0xf8u, 0xfcu, 0xfeu, 0xffu}) {
        FakeGic fake;
        fake.priorityBits = bits;
        Controller controller;
        prepared(fake, controller);
        expect(controller.enableLocal(), "valid four-through-eight-bit priority mask enables");
        expect(fake.cpu[1] == bits, "PMR retains only hardware-implemented priority bits");
        expect(controller.priorityBits() == bits && controller.priorityMaskBits() == bits &&
            controller.priorityMaskEnabled(), "priority precision is measured rather than assumed");
        expect(controller.configureSpi(40, false, 0xa9), "SPI priority rounds to implemented bits");
        expect(!controller.configureSpi(40, false, static_cast<u8>(bits)),
            "a priority masked by implemented PMR precision is refused");
    }
    for (u32 bits : {0u, 0x80u, 0xe0u, 0xf4u, 0xfau}) {
        FakeGic fake;
        fake.priorityBits = bits;
        Controller controller;
        expect(controller.configure(fake.io(), 4096, 4096) && controller.prepareDistributor(),
            "invalid priority precision fixture reaches local setup");
        expect(!controller.prepareLocal() && !controller.enableLocal(),
            "missing or noncontiguous priority precision cannot enable interrupts");
    }
    FakeGic frozen;
    frozen.constantPriorityMask = 0xf0;
    Controller frozenController;
    expect(frozenController.configure(frozen.io(), 4096, 4096) && frozenController.prepareDistributor(),
        "frozen PMR fixture reaches local setup");
    expect(!frozenController.prepareLocal(), "plausible constant PMR must fail the writable-pattern proof");
    const auto failure = frozenController.failure();
    expect(failure.check == Check::Readback && failure.space == Space::Cpu && failure.offset == 4 &&
        failure.expected == 0x50 && failure.observed == 0xf0 && failure.mask == 0xff,
        "rejected register records its exact space, offset, expectation and observation");
    frozenController.disableLocal();
    expect(frozenController.failure().observed == 0xf0, "quiesce cannot erase the original failing readback");
}
}

int main()
{
    testCompatibility();
    testViews();
    testSpi();
    testRefusals();
    testPriorityPrecision();
    std::printf("GicV2Core: %u checks, %u failures (simulated MMIO, not hardware)\n", checks, failures);
    return failures ? 1 : 0;
}
