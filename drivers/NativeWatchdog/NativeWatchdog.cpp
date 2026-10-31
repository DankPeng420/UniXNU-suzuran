// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// IOWatchdog stand-in for the Lumia 950 (no Apple SoC watchdog, no Apple PMU).
//
// watchdogd opens IOServiceMatching("IOWatchdog") with type 1 and crashes on purpose when it is
// missing ("failed to discover watchdog KEXT service"); it also looks up the IOService whose
// IOPMUPrimary is 1 to record the boot stage there, and crashes when none exists (customer builds
// ignore -wdt_no_pmu). On Apple hardware the kext arms a SoC watchdog that resets the device unless
// watchdogd keeps checking in. This phone has no reset path from XNU, so the service accepts every
// call (check-in, enable/disable userspace monitoring, the struct calls), zeroes the outputs and
// arms nothing. The selector set and the IOPMUPrimary requirement follow ios-sweet's hardware notes
// (github.com/dipa-studio/ios-sweet, patches 0040/0045). Each selector is logged once.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSNumber.h>

namespace {
uint64_t gLogged;   // selectors below 64 already logged
}

class IOWatchdogUserClient final : public IOUserClient {
    OSDeclareDefaultStructors(IOWatchdogUserClient)
public:
    IOReturn clientClose() override { terminate(); return kIOReturnSuccess; }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                            OSObject *, void *) override
    {
        if (!args) return kIOReturnBadArgument;
        for (uint32_t i = 0; i < args->scalarOutputCount; ++i) args->scalarOutput[i] = 0;
        if (args->structureOutputDescriptor) {
            static const uint8_t zero[64] = {};
            const IOByteCount length = args->structureOutputDescriptor->getLength();
            for (IOByteCount offset = 0; offset < length; offset += sizeof zero) {
                const IOByteCount chunk = length - offset < sizeof zero ? length - offset : sizeof zero;
                args->structureOutputDescriptor->writeBytes(offset, zero, chunk);
            }
        } else if (args->structureOutput && args->structureOutputSize) {
            bzero(args->structureOutput, args->structureOutputSize);
        }
        if (selector >= 64 || !(gLogged & (1ULL << selector))) {
            if (selector < 64) gLogged |= 1ULL << selector;
            IOLog("NativeWDT: sel %u in %u/%u out %u -> ok\n", selector, args->scalarInputCount,
                  args->structureInputDescriptor ? (uint32_t)args->structureInputDescriptor->getLength()
                                                 : args->structureInputSize,
                  args->scalarOutputCount);
        }
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(IOWatchdogUserClient, IOUserClient)

class IOWatchdog final : public IOService {
    OSDeclareDefaultStructors(IOWatchdog)
public:
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider)) return false;
        setProperty(kIOUserClientClassKey, "IOWatchdogUserClient");
        setProperty("IOPMUPrimary", 1ULL, 32);
        registerService();
        IOLog("NativeWDT: IOWatchdog registered (accepts check-ins, never fires)\n");
        return true;
    }
    // watchdogd writes its boot stage here (IORegistryEntrySetCFProperty); only a failure is logged.
    IOReturn setProperties(OSObject *) override { return kIOReturnSuccess; }
};
OSDefineMetaClassAndStructors(IOWatchdog, IOService)
