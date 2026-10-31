// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The battery for iOS on the Lumia 950 XL: an IOPMPowerSource, which powerd matches and reads
// (CurrentCapacity, IsCharging, ExternalConnected, ...) for the status bar and Settings > Battery.
//
// The values come from the PMI8994 (SID 2) over NativeTouch's SPMI channel (native_spmi_read),
// read-only (cityman kernel, qpnp-smbcharger.c and qpnp-fg.c):
//   - charge level: the fuel gauge's monotonic state of charge (FG_SOC 0x4009, 0-255),
//   - charger: USB_CHGPTH RT_STS bit 2 (input detected), CHGR_STS bits 2:1 (0 not charging,
//     1 pre-charge, 2 fast, 3 taper) and bit 0 (charging enabled),
//   - the input current limit (USB_CHGPTH IL_CFG) for the log.
// Charging itself needs nothing from iOS: the charger keeps the firmware's setup (boot 14,
// 2026-10-31: 1500 mA input limit in high-current mode, fast charge then taper to full).
// The battery is the BV-T4D, 3340 mAh (microsoft_bvt4d_3340mah.dtsi). Polled every 20 s; each
// change is logged. Boot-arg nativebattery=0 leaves it unpublished.
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <kern/thread.h>

extern "C" bool native_spmi_read(uint8_t slave, uint16_t address, uint8_t *value) __attribute__((weak));

namespace {

constexpr uint8_t kPmi = 2;
constexpr uint16_t kChgr = 0x1000, kUsbChgpth = 0x1300, kFgSoc = 0x4000;
constexpr uint32_t kDesignMAh = 3340;
constexpr uint32_t kUsbCurrentMA[] = {300, 400, 450, 475, 500, 550, 600, 650, 700, 900, 950, 1000, 1100, 1200, 1400,
                                      1450, 1500, 1600, 1800, 1850, 1880, 1910, 1930, 1950, 1970, 2000, 2050, 2100,
                                      2300, 2400, 2500, 3000};

struct Reading {
    bool external, charging, done;
    uint32_t percent, state, limitMA;
    bool operator!=(const Reading &o) const
    {
        return external != o.external || charging != o.charging || done != o.done || percent != o.percent ||
            state != o.state;
    }
};

bool read(Reading &r)
{
    uint8_t usbRt = 0, chgrSts = 0, ilCfg = 0, soc = 0;
    if (!native_spmi_read || !native_spmi_read(kPmi, kUsbChgpth + 0x10, &usbRt) ||
        !native_spmi_read(kPmi, kChgr + 0x0e, &chgrSts) || !native_spmi_read(kPmi, kUsbChgpth + 0xf2, &ilCfg) ||
        !native_spmi_read(kPmi, kFgSoc + 0x09, &soc)) {
        return false;
    }
    r.external = (usbRt & 4) != 0;
    r.state = (chgrSts >> 1) & 3;
    r.charging = r.external && r.state != 0;
    r.percent = (soc * 100u + 127) / 255;
    r.done = r.external && !r.charging && r.percent >= 99;
    r.limitMA = kUsbCurrentMA[ilCfg & 31];
    return true;
}

}

class NativeBattery final : public IOPMPowerSource {
    OSDeclareDefaultStructors(NativeBattery)
public:
    // The catalogue instantiates through init(OSDictionary *), which skips IOPMPowerSource::init()
    // and leaves its property dictionary and keys unset (setPSProperty faulted on a null dictionary,
    // boot 16). Run that init, then carry the personality's properties over.
    bool init(OSDictionary *dictionary) override
    {
        if (!IOPMPowerSource::init()) return false;
        OSCollectionIterator *keys = dictionary ? OSCollectionIterator::withCollection(dictionary) : nullptr;
        while (OSSymbol *key = keys ? OSDynamicCast(OSSymbol, keys->getNextObject()) : nullptr) {
            setProperty(key, dictionary->getObject(key));
        }
        OSSafeReleaseNULL(keys);
        return true;
    }

    bool start(IOService *provider) override
    {
        uint32_t enabled = 1;
        PE_parse_boot_argn("nativebattery", &enabled, sizeof enabled);
        if (!enabled) {
            IOLog("NativeBattery: not published (nativebattery=0)\n");
            return false;
        }
        if (!IOPMPowerSource::start(provider)) return false;
        setBatteryInstalled(true);
        setExternalChargeCapable(true);
        setMaxCapacity(100);
        setCurrentCapacity(100);
        setNumber("DesignCapacity", kDesignMAh);
        setNumber("NominalChargeCapacity", kDesignMAh);
        setNumber("AppleRawMaxCapacity", kDesignMAh);
        updateStatus();
        registerService();
        thread_t thread = nullptr;
        if (kernel_thread_start(pollThread, this, &thread) != KERN_SUCCESS) {
            IOLog("NativeBattery: no poll thread\n");
            return true;
        }
        thread_deallocate(thread);
        IOLog("NativeBattery: published, polling every 20 s\n");
        return true;
    }

private:
    void setNumber(const char *key, uint32_t value)
    {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *number = OSNumber::withNumber(value, 32);
        if (symbol && number) setPSProperty(symbol, number);
        OSSafeReleaseNULL(number);
        OSSafeReleaseNULL(symbol);
    }

    void setFlag(const char *key, bool value)
    {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }

    static void pollThread(void *owner, wait_result_t)
    {
        NativeBattery *self = static_cast<NativeBattery *>(owner);
        Reading last = {};
        bool first = true;
        uint32_t failures = 0;
        for (;; IOSleep(20000)) {
            Reading now = {};
            if (!read(now)) {
                if (failures++ < 3) IOLog("NativeBattery: PMI8994 read failed\n");
                continue;
            }
            if (!first && !(now != last)) continue;
            static const char *const kState[] = {"not charging", "pre-charge", "fast charge", "taper"};
            IOLog("NativeBattery: %u%%, usb %s, %s%s, input limit %u mA\n", now.percent,
                  now.external ? "present" : "absent", kState[now.state], now.done ? " (full)" : "", now.limitMA);
            self->apply(now);
            last = now;
            first = false;
        }
    }

    void apply(const Reading &r)
    {
        setExternalConnected(r.external);
        setIsCharging(r.charging);
        setCurrentCapacity(r.percent);
        setFlag("FullyCharged", r.done);
        setFlag("AppleRawExternalConnected", r.external);
        setNumber("AppleRawCurrentCapacity", r.percent * kDesignMAh / 100);
        setAtWarnLevel(r.percent <= 20);
        setAtCriticalLevel(r.percent <= 5);
        updateStatus();
    }
};
OSDefineMetaClassAndStructors(NativeBattery, IOPMPowerSource)
