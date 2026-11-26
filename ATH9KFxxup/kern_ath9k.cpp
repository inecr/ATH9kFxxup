//
//  kern_ath9k.cpp
//  ATH9KFxxup
//
//  Copyright © 2017 lvs1974. All rights reserved.
//
//  This kext is made based on lvs1974's NvidiaGraphicsFixup.
//

#include <Headers/kern_api.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOCatalogue.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#include "kern_ath9k.hpp"
#include "kern_ath9300.h"
#include "kern_ath946x.h"
#include "kern_ath9485.h"
#include "kern_ath9565.h"

static const char *kextAirPortAtheros40[] {
    "/System/Library/Extensions/IO80211Family.kext/Contents/PlugIns/AirPortAtheros40.kext/Contents/MacOS/AirPortAtheros40"
};

static KernelPatcher::KextInfo kextList[] {
    { "com.apple.driver.AirPort.Atheros40", kextAirPortAtheros40, 1, {true, true, false, false, true}, {}, KernelPatcher::KextInfo::Unloaded }
};

static size_t kextListSize {1};

static thread_call_t gRematchCall {nullptr};
static bool gAllowAtherosMatch {false};
static ATH9KDelayStub *gStubInstance {nullptr};
static mach_vm_address_t gAirportInstanceAddr {0};

OSDefineMetaClassAndStructors(ATH9KDelayStub, IOService)

IOService *ATH9KDelayStub::probe(IOService *provider, SInt32 *score) {
    if (gAllowAtherosMatch)
        return nullptr;
    return IOService::probe(provider, score);
}

bool ATH9KDelayStub::start(IOService *provider) {
    if (gAllowAtherosMatch)
        return false;
    if (!IOService::start(provider))
        return false;
    gStubInstance = this;
    SYSLOG("ath9k", "ATH9KDelayStub holding Atheros PCI device until Lilu is ready");
    return true;
}

void ATH9KDelayStub::stop(IOService *provider) {
    if (gStubInstance == this)
        gStubInstance = nullptr;
    IOService::stop(provider);
}

bool ATH9K::init() {
    // So that user can identify successful kext injection.
    SYSLOG("ATH9KFxxup", "Kext loaded. Patching will start now.\n");

	LiluAPI::Error error = lilu.onKextLoad(kextList, kextListSize,
	[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
        ATH9K *ath9k = static_cast<ATH9K *>(user);
        ath9k->processKext(patcher, index, address, size);
	}, this);

	if (error != LiluAPI::Error::NoError) {
		SYSLOG("ath9k", "failed to register onKextLoad method %d", error);
		return false;
	}

	return true;
}

static void rematchAfterPatch(thread_call_param_t, thread_call_param_t) {
    gAllowAtherosMatch = true;

    if (gAirportInstanceAddr) {
        auto *instancePtr = reinterpret_cast<void **>(gAirportInstanceAddr);
        SYSLOG("ath9k", "rematchAfterPatch resetting _airportInstance (was %p) to nullptr", *instancePtr);
        *instancePtr = nullptr;
    }

    if (gStubInstance) {
        ATH9KDelayStub *stub = gStubInstance;
        IOService *provider = stub->getProvider();
        if (provider)
            provider->retain();

        SYSLOG("ath9k", "rematchAfterPatch releasing ATH9KDelayStub");
        gStubInstance = nullptr;
        stub->terminate(kIOServiceRequired);

        if (provider) {
            provider->registerService();
            provider->release();
        }
        return;
    }

    if (OSDictionary *matching = IOService::serviceMatching("IOPCIDevice")) {
        if (OSIterator *iter = IOService::getMatchingServices(matching)) {
            while (OSObject *obj = iter->getNextObject()) {
                IOService *device = OSDynamicCast(IOService, obj);
                if (!device) continue;

                OSData *vendorData = OSDynamicCast(OSData, device->getProperty("vendor-id"));
                if (!vendorData || vendorData->getLength() < sizeof(uint16_t)) continue;

                const uint16_t vendorId = *static_cast<const uint16_t *>(vendorData->getBytesNoCopy());
                if (vendorId != 0x168c) continue;

                bool hasClient = false;
                if (OSIterator *clients = device->getClientIterator()) {
                    hasClient = (clients->getNextObject() != nullptr);
                    clients->release();
                }

                SYSLOG("ath9k", "rematchAfterPatch found 0x168c device, hasClient=%d", hasClient ? 1 : 0);
                if (!hasClient) {
                    SYSLOG("ath9k", "rematchAfterPatch calling registerService()");
                    device->registerService();
                }
            }
            iter->release();
        }
        matching->release();
    }
}

void ATH9K::deinit() {
    if (gRematchCall) {
        thread_call_cancel(gRematchCall);
        thread_call_free(gRematchCall);
        gRematchCall = nullptr;
    }
}

void ATH9K::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
    if (progressState != ProcessingState::EverythingDone) {
        for (size_t i = 0; i < kextListSize; i++) {
            if (kextList[i].loadIndex == index) {
                if (!(progressState & ProcessingState::AirPortAtheros40Patched) && !strcmp(kextList[i].id, "com.apple.driver.AirPort.Atheros40")) {
                    SYSLOG("ath9k", "found com.apple.driver.AirPort.Atheros40 (index %zu, addr 0x%llx, size 0x%zx)", index, address, size);

                    gAirportInstanceAddr = patcher.solveSymbol(index, "_airportInstance");
                    if (gAirportInstanceAddr) {
                        SYSLOG("ath9k", "solved _airportInstance at 0x%llx", gAirportInstanceAddr);
                    } else {
                        SYSLOG("ath9k", "failed to solve _airportInstance (error %d)", patcher.getError());
                        patcher.clearError();
                    }

                    if (getKernelVersion() == KernelVersion::Mavericks){
                        const uint8_t find1[]    = {0x66, 0x89, 0x83, 0x20, 0x02, 0x00, 0x00};
                        const uint8_t replace1[] = {0xC6, 0x83, 0x20, 0x02, 0x00, 0x00, 0x30};
                        KextPatch kext_patch1 {
                            {&kextList[i], find1, replace1, sizeof(find1), 1},
                            KernelVersion::Mavericks, KernelVersion::Mavericks
                        };
                        applyPatches(patcher, index, &kext_patch1, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", "patched 10.9.x start");

                        const uint8_t find2[]    = {0x66, 0x83, 0xFA, 0x30};
                        const uint8_t replace2[] = {0x66, 0x83, 0xFA, 0x00};
                        KextPatch kext_patch2 {
                            {&kextList[i], find2, replace2, sizeof(find2), 1},
                            KernelVersion::Mavericks, KernelVersion::Mavericks
                        };
                        applyPatches(patcher, index, &kext_patch2, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", "patched ___ath_attach");
                    }
                    else if (getKernelVersion() == KernelVersion::Yosemite){
                        const uint8_t find1[]    = {0x66, 0x89, 0x83, 0x48, 0x02, 0x00, 0x00};
                        const uint8_t replace1[] = {0xC6, 0x83, 0x48, 0x02, 0x00, 0x00, 0x30};
                        KextPatch kext_patch1 {
                            {&kextList[i], find1, replace1, sizeof(find1), 1},
                            KernelVersion::Yosemite, KernelVersion::Yosemite
                        };
                        applyPatches(patcher, index, &kext_patch1, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", "patched 10.10.x start");

                        const uint8_t find2[]    = {0x41, 0x0F, 0xB7, 0xFE, 0x83, 0xFF, 0x30};
                        const uint8_t replace2[] = {0x41, 0x0F, 0xB7, 0xFE, 0x83, 0xFF, 0x00};
                        KextPatch kext_patch2 {
                            {&kextList[i], find2, replace2, sizeof(find2), 1},
                            KernelVersion::Yosemite, KernelVersion::Yosemite
                        };
                        applyPatches(patcher, index, &kext_patch2, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k" ,"patched ___ath_attach");
                    }
                    else if (getKernelVersion() == KernelVersion::ElCapitan){
                        const uint8_t find1[]    = {0x66, 0x89, 0x83, 0x10, 0x04, 0x00, 0x00};
                        const uint8_t replace1[] = {0xC6, 0x83, 0x10, 0x04, 0x00, 0x00, 0x30};
                        KextPatch kext_patch1 {
                            {&kextList[i], find1, replace1, sizeof(find1), 1},
                            KernelVersion::ElCapitan, KernelVersion::ElCapitan
                        };
                        applyPatches(patcher, index, &kext_patch1, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k" ,"patched 10.11.x start");

                        const uint8_t find2[]    = {0x0F, 0xB7, 0xF8, 0x83, 0xFF, 0x30};
                        const uint8_t replace2[] = {0x0F, 0xB7, 0xF8, 0x83, 0xFF, 0x00};
                        KextPatch kext_patch2 {
                            {&kextList[i], find2, replace2, sizeof(find2), 1},
                            KernelVersion::ElCapitan, KernelVersion::ElCapitan
                        };
                        applyPatches(patcher, index, &kext_patch2, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", "patched ___ath_attach");
                    }
                    else if (getKernelVersion() == KernelVersion::Sierra){
                        const uint8_t find1[]    = {0x66, 0x89, 0x83, 0x38, 0x04, 0x00, 0x00};
                        const uint8_t replace1[] = {0xC6, 0x83, 0x38, 0x04, 0x00, 0x00, 0x30};
                        KextPatch kext_patch1 {
                            {&kextList[i], find1, replace1, sizeof(find1), 1},
                            KernelVersion::Sierra, KernelVersion::Sierra
                        };
                        applyPatches(patcher, index, &kext_patch1, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", "patched 10.12.x start");

                        const uint8_t find2[]    = {0x0F, 0xB7, 0xF8, 0x83, 0xFF, 0x30};
                        const uint8_t replace2[] = {0x0F, 0xB7, 0xF8, 0x83, 0xFF, 0x00};
                        KextPatch kext_patch2 {
                            {&kextList[i], find2, replace2, sizeof(find2), 1},
                            KernelVersion::Sierra, KernelVersion::Sierra
                        };
                        applyPatches(patcher, index, &kext_patch2, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", "patched ___ath_attach");
                    }
                    else if (getKernelVersion() == KernelVersion::HighSierra){
                        const uint8_t find1[]    = {0x66, 0x89, 0x83, 0x38, 0x04, 0x00, 0x00};
                        const uint8_t replace1[] = {0xC6, 0x83, 0x38, 0x04, 0x00, 0x00, 0x30};
                        KextPatch kext_patch1 {
                            {&kextList[i], find1, replace1, sizeof(find1), 1},
                            KernelVersion::HighSierra, KernelVersion::HighSierra
                        };
                        applyPatches(patcher, index, &kext_patch1, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched 10.13.x start");
                        
                        const uint8_t find2[]    = {0x41, 0x0F, 0xB7, 0xFD, 0x83, 0xFF, 0x30};
                        const uint8_t replace2[] = {0x41, 0x0F, 0xB7, 0xFD, 0x83, 0xFF, 0x00};
                        KextPatch kext_patch2 {
                            {&kextList[i], find2, replace2, sizeof(find2), 1},
                            KernelVersion::HighSierra, KernelVersion::HighSierra
                        };
                        applyPatches(patcher, index, &kext_patch2, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched ___ath_attach");

                        // Fallback only if _airportInstance symbol could not be solved:
                        // bypass "can't support 2 instances - bye..." in probe() (je -> jmp).
                        if (!gAirportInstanceAddr) {
                            const uint8_t find_2inst[]    = {0x31, 0xC0, 0x48, 0x89, 0x83, 0xF0, 0x03, 0x00, 0x00, 0x48, 0x39, 0x05, 0x41, 0x9F, 0x13, 0x00, 0x74, 0x16};
                            const uint8_t replace_2inst[] = {0x31, 0xC0, 0x48, 0x89, 0x83, 0xF0, 0x03, 0x00, 0x00, 0x48, 0x39, 0x05, 0x41, 0x9F, 0x13, 0x00, 0xEB, 0x16};
                            KextPatch kext_patch_2inst {
                                {&kextList[i], find_2inst, replace_2inst, sizeof(find_2inst), 1},
                                KernelVersion::HighSierra, KernelVersion::HighSierra
                            };
                            applyPatches(patcher, index, &kext_patch_2inst, 1);
                            SYSLOG("ath9k", "applied 2-instance bypass fallback patch");
                        }
                    }
                    
                    // Patch is same as macOS High Sierra (as of now), Just adding patcher support for Mojave
                    else if (getKernelVersion() == KernelVersion::Mojave){
                        const uint8_t find1[]    = {0x66, 0x89, 0x83, 0x38, 0x04, 0x00, 0x00};
                        const uint8_t replace1[] = {0xC6, 0x83, 0x38, 0x04, 0x00, 0x00, 0x30};
                        KextPatch kext_patch1 {
                            {&kextList[i], find1, replace1, sizeof(find1), 1},
                            KernelVersion::Mojave, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch1, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", "patched 10.14.x start");
                        
                        const uint8_t find2[]    = {0x41, 0x0F, 0xB7, 0xFD, 0x83, 0xFF, 0x30};
                        const uint8_t replace2[] = {0x41, 0x0F, 0xB7, 0xFD, 0x83, 0xFF, 0x00};
                        KextPatch kext_patch2 {
                            {&kextList[i], find2, replace2, sizeof(find2), 1},
                            KernelVersion::Mojave, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch2, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", "patched ___ath_attach");
                    }

                    const uint8_t find3[]    = {0xC1, 0xE9, 0x0C, 0x81, 0xE1, 0xC0, 0xFF, 0x0F, 0x00};
                    const uint8_t replace3[] = {0xC1, 0xE9, 0x0C, 0x90, 0xB9, 0xC0, 0x01, 0x00, 0x00};
                    KextPatch kext_patch3 {
                        {&kextList[i], find3, replace3, sizeof(find3), 4},
                        KernelVersion::Mavericks, KernelVersion::Mojave
                    };
                    applyPatches(patcher, index, &kext_patch3, 1);
                    progressState |= ProcessingState::AirPortAtheros40Patched;
                    DBGLOG("ath9k", " patched _ar9300ReadRevisions");

                    const uint8_t find4[]    = {0xC1, 0xE8, 0x0C, 0x25, 0xC0, 0xFF, 0x0F, 0x00};
                    const uint8_t replace4[] = {0xC1, 0xE8, 0x0C, 0xB8, 0xC0, 0x01, 0x00, 0x00};
                    KextPatch kext_patch4 {
                        {&kextList[i], find4, replace4, sizeof(find4), 2},
                        KernelVersion::Mavericks, KernelVersion::Mavericks
                    };
                    applyPatches(patcher, index, &kext_patch4, 1);
                    progressState |= ProcessingState::AirPortAtheros40Patched;
                    DBGLOG("ath9k", " patched _ar9300ReadRevisions #1");

                    const uint8_t find5[]    = {0xC1, 0xE9, 0x08, 0x83, 0xE1, 0x0F};
                    const uint8_t replace5[] = {0x90, 0xB9, 0x02, 0x00, 0x00, 0x00};
                    KextPatch kext_patch5 {
                        {&kextList[i], find5, replace5, sizeof(find5), 5},
                        KernelVersion::Mavericks, KernelVersion::Mojave
                    };
                    applyPatches(patcher, index, &kext_patch5, 1);
                    progressState |= ProcessingState::AirPortAtheros40Patched;
                    DBGLOG("ath9k", " patched _ar9300ReadRevisions #2");

                    const uint8_t find6[]    = {0xC1, 0xE8, 0x08, 0x83, 0xE0, 0x0F};
                    const uint8_t replace6[] = {0x90, 0xB8, 0x02, 0x00, 0x00, 0x00};
                    KextPatch kext_patch6 {
                        {&kextList[i], find6, replace6, sizeof(find6), 3},
                        KernelVersion::Mavericks, KernelVersion::Mojave
                    };
                    applyPatches(patcher, index, &kext_patch6, 1);
                    progressState |= ProcessingState::AirPortAtheros40Patched;
                    DBGLOG("ath9k", " patched _ar9300ReadRevisions #3");

                    const uint8_t find7[]    = {0x0F, 0xB6, 0x87, 0x00, 0xB2, 0x00, 0x00, 0x83, 0xE0, 0x0F};
                    const uint8_t replace7[] = {0x0F, 0xB6, 0x87, 0x00, 0xB2, 0x00, 0x00, 0x90, 0x31, 0xC0};
                    KextPatch kext_patch7 {
                        {&kextList[i], find7, replace7, sizeof(find7), 1},
                        KernelVersion::Mavericks, KernelVersion::Mojave
                    };
                    applyPatches(patcher, index, &kext_patch7, 1);
                    progressState |= ProcessingState::AirPortAtheros40Patched;
                    DBGLOG("ath9k", " patched _ar9300RxGainIndexGet");

                    const uint8_t find8[]    = {0x0F, 0xB6, 0x87, 0x00, 0xB2, 0x00, 0x00, 0xC1, 0xE8, 0x04};
                    const uint8_t replace8[] = {0x0F, 0xB6, 0x87, 0x00, 0xB2, 0x00, 0x00, 0x90, 0x31, 0xC0};
                    KextPatch kext_patch8 {
                        {&kextList[i], find8, replace8, sizeof(find8), 1},
                        KernelVersion::Mavericks, KernelVersion::Mojave
                    };
                    applyPatches(patcher, index, &kext_patch8, 1);
                    progressState |= ProcessingState::AirPortAtheros40Patched;
                    DBGLOG("ath9k", " patched _ar9300TxGainIndexGet");

                    const uint8_t find9[]    = {0x66, 0x89, 0x83, 0x16, 0x09, 0x00, 0x00};
                    const uint8_t replace9[] = {0xC6, 0x83, 0x16, 0x09, 0x00, 0x00, 0x00};
                    KextPatch kext_patch9 {
                        {&kextList[i], find9, replace9, sizeof(find9), 4},
                        KernelVersion::Mavericks, KernelVersion::Mojave
                    };
                    applyPatches(patcher, index, &kext_patch9, 1);
                    progressState |= ProcessingState::AirPortAtheros40Patched;
                    DBGLOG("ath9k", " patched _ar9300FillCapabilityInfo");
 
                    const uint8_t find10[]    = {0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x10, 0x00};
                    const uint8_t replace10[] = {0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x10, 0x00};
                    KextPatch kext_patch10 {
                        {&kextList[i], find10, replace10, sizeof(find10), 1},
                        KernelVersion::Mavericks, KernelVersion::Mojave
                    };
                    applyPatches(patcher, index, &kext_patch10, 1);
                    progressState |= ProcessingState::AirPortAtheros40Patched;
                    DBGLOG("ath9k", " patched _ar9300FillCapabilityInfo #2");

                    char tmp[16];
                    if (PE_parse_boot_argn("-ath9485", tmp, sizeof(tmp))) {
                        const uint8_t find[]    = {0x39, 0x33, 0x38, 0x30};
                        const uint8_t replace[] = {0x39, 0x34, 0x38, 0x35};
                        KextPatch kext_patch {
                            {&kextList[i], find, replace, sizeof(find), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched AR9485");

                        if (getKernelVersion() == KernelVersion::Mavericks){
                            const uint8_t find11[]    = {0xFF, 0x03, 0x00, 0x00, 0x4C, 0x89, 0xFF, 0x4C, 0x89, 0xF6, 0x89, 0xDA, 0xB9, 0xFF, 0x03, 0x00, 0x00};
                            const uint8_t replace11[] = {0xFF, 0x01, 0x00, 0x00, 0x4C, 0x89, 0xFF, 0x4C, 0x89, 0xF6, 0x89, 0xDA, 0xB9, 0xFF, 0x01, 0x00, 0x00};
                            KextPatch kext_patch11 {
                                {&kextList[i], find11, replace11, sizeof(find11), 2},
                                KernelVersion::Mavericks, KernelVersion::Mavericks
                            };
                            applyPatches(patcher, index, &kext_patch11, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300EepromRestoreInternal");

                            const uint8_t find12[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0xFF, 0xD0};
                            const uint8_t replace12[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xD0};
                            KextPatch kext_patch12 {
                                {&kextList[i], find12, replace12, sizeof(find12), 2},
                                KernelVersion::Mavericks, KernelVersion::Mavericks
                            };
                            applyPatches(patcher, index, &kext_patch12, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset");

                            const uint8_t find13[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0xFF, 0x13};
                            const uint8_t replace13[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x13};
                            KextPatch kext_patch13 {
                                {&kextList[i], find13, replace13, sizeof(find13), 2},
                                KernelVersion::Mavericks, KernelVersion::Mavericks
                            };
                            applyPatches(patcher, index, &kext_patch13, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset #2");
                        }
                        else {
                            const uint8_t find11[]    = {0xFF, 0x03, 0x00, 0x00, 0xB9, 0xFF, 0x03, 0x00, 0x00};
                            const uint8_t replace11[] = {0xFF, 0x01, 0x00, 0x00, 0xB9, 0xFF, 0x01, 0x00, 0x00};
                            KextPatch kext_patch11 {
                                {&kextList[i], find11, replace11, sizeof(find11), 2},
                                KernelVersion::Yosemite, KernelVersion::HighSierra
                            };
                            applyPatches(patcher, index, &kext_patch11, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300EepromRestoreInternal");

                            const uint8_t find12[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0xFF, 0xD0};
                            const uint8_t replace12[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0xFF, 0xD0};
                            KextPatch kext_patch12 {
                                {&kextList[i], find12, replace12, sizeof(find12), 2},
                                KernelVersion::Yosemite, KernelVersion::HighSierra
                            };
                            applyPatches(patcher, index, &kext_patch12, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset");

                            const uint8_t find13[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0x41, 0xFF, 0x55, 0x00};
                            const uint8_t replace13[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0x41, 0xFF, 0x55, 0x00};
                            KextPatch kext_patch13 {
                                {&kextList[i], find13, replace13, sizeof(find13), 2},
                                KernelVersion::Yosemite, KernelVersion::HighSierra
                            };
                            applyPatches(patcher, index, &kext_patch13, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset #2");
                        }
/*
                        const uint8_t find12[]    = {0xA2, 0x00, 0x00, 0xBA, 0x07, 0x00, 0x00, 0x00};
                        const uint8_t replace12[] = {0xA2, 0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x00};
                        KextPatch kext_patch12 {
                            {&kextList[i], find12, replace12, sizeof(find12), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch12, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Reset");

                        const uint8_t find13[]    = {0x83, 0x00, 0x00, 0xBA, 0x07, 0x00, 0x00, 0x00};
                        const uint8_t replace13[] = {0x83, 0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x00};
                        KextPatch kext_patch13 {
                            {&kextList[i], find13, replace13, sizeof(find13), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch13, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Reset #2");
*/
                        const uint8_t find14[]    = {0xBE, 0x88, 0x62, 0x01, 0x00};
                        const uint8_t replace14[] = {0xBE, 0x84, 0x62, 0x01, 0x00};
                        KextPatch kext_patch14 {
                            {&kextList[i], find14, replace14, sizeof(find14), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch14, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300 #1");

                        const uint8_t find15[]    = {0xBE, 0x90, 0x62, 0x01, 0x00};
                        const uint8_t replace15[] = {0xBE, 0x8C, 0x62, 0x01, 0x00};
                        KextPatch kext_patch15 {
                            {&kextList[i], find15, replace15, sizeof(find15), 4},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch15, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300 #2");

                        KextPatch kext_patch51 {
                            {&kextList[i], _ar9300_osprey_2p0_mac_core, _ar9485_osprey_2p0_mac_core, sizeof(_ar9300_osprey_2p0_mac_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch51, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_mac_core");

                        KextPatch kext_patch52 {
                            {&kextList[i], _ar9300_osprey_2p0_mac_postamble, _ar9485_osprey_2p0_mac_postamble, sizeof(_ar9300_osprey_2p0_mac_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch52, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_mac_postamble");

                        KextPatch kext_patch53 {
                            {&kextList[i], _ar9300_osprey_2p0_baseband_core, _ar9485_osprey_2p0_baseband_core, sizeof(_ar9300_osprey_2p0_baseband_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch53, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_baseband_core");

                        KextPatch kext_patch54 {
                            {&kextList[i], _ar9300_osprey_2p0_baseband_postamble, _ar9485_osprey_2p0_baseband_postamble, sizeof(_ar9300_osprey_2p0_baseband_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch54, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_baseband_postamble");

                        KextPatch kext_patch55 {
                            {&kextList[i], _ar9300_osprey_2p0_radio_core, _ar9485_osprey_2p0_radio_core, sizeof(_ar9300_osprey_2p0_radio_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch55, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_radio_core");

                        KextPatch kext_patch56 {
                            {&kextList[i], _ar9300_osprey_2p0_radio_postamble, _ar9485_osprey_2p0_radio_postamble, sizeof(_ar9300_osprey_2p0_radio_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch56, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_radio_postamble");

                        KextPatch kext_patch57 {
                            {&kextList[i], _ar9300_osprey_2p0_soc_preamble, _ar9485_osprey_2p0_soc_preamble, sizeof(_ar9300_osprey_2p0_soc_preamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch57, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_soc_preamble");

                        KextPatch kext_patch58 {
                            {&kextList[i], _ar9300_osprey_2p0_soc_postamble, _ar9485_osprey_2p0_soc_postamble, sizeof(_ar9300_osprey_2p0_soc_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch58, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_soc_postamble");

                        KextPatch kext_patch59 {
                            {&kextList[i], _ar9300Common_rx_gain_table_osprey_2p0, _ar9485Common_rx_gain_table_osprey_2p0, sizeof(_ar9300Common_rx_gain_table_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch59, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Common_rx_gain_table_osprey_2p0");

                        KextPatch kext_patch60 {
                            {&kextList[i], _ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0, _ar9485Modes_lowest_ob_db_tx_gain_table_osprey_2p0, sizeof(_ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch60, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0");

                        KextPatch kext_patch61 {
                            {&kextList[i], _ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0, _ar9485PciePhy_pll_on_clkreq_disable_L1_osprey_2p0, sizeof(_ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch61, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0");

                        KextPatch kext_patch62 {
                            {&kextList[i], _ar9300PciePhy_clkreq_enable_L1_osprey_2p0, _ar9485PciePhy_clkreq_enable_L1_osprey_2p0, sizeof(_ar9300PciePhy_clkreq_enable_L1_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch62, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300PciePhy_clkreq_enable_L1_osprey_2p0");

                        KextPatch kext_patch63 {
                            {&kextList[i], _ar9300Modes_fast_clock_osprey_2p0, _ar9485Modes_fast_clock_osprey_2p0, sizeof(_ar9300Modes_fast_clock_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch63, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Modes_fast_clock_osprey_2p0");
                     }
                    else if (PE_parse_boot_argn("-ath9565", tmp, sizeof(tmp))) {
                        const uint8_t find[]    = {0x39, 0x33, 0x38, 0x30};
                        const uint8_t replace[] = {0x39, 0x35, 0x36, 0x35};
                        KextPatch kext_patch {
                            {&kextList[i], find, replace, sizeof(find), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched AR9565");

                        const uint8_t find11[]    = {0x8D, 0x90, 0x00, 0x04, 0x00, 0x00};
                        const uint8_t replace11[] = {0x8D, 0x90, 0x00, 0x04, 0x08, 0x00};
                        KextPatch kext_patch11 {
                            {&kextList[i], find11, replace11, sizeof(find11), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch11, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300SetRxFilter");

                        if (getKernelVersion() == KernelVersion::Mavericks){
                            const uint8_t find12[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0xFF, 0xD0};
                            const uint8_t replace12[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xD0};
                            KextPatch kext_patch12 {
                                {&kextList[i], find12, replace12, sizeof(find12), 2},
                                KernelVersion::Mavericks, KernelVersion::Mavericks
                            };
                            applyPatches(patcher, index, &kext_patch12, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset");

                            const uint8_t find13[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0xFF, 0x13};
                            const uint8_t replace13[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x13};
                            KextPatch kext_patch13 {
                                {&kextList[i], find13, replace13, sizeof(find13), 2},
                                KernelVersion::Mavericks, KernelVersion::Mavericks
                            };
                            applyPatches(patcher, index, &kext_patch13, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset #2");
                        }
                        else {
                            const uint8_t find12[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0xFF, 0xD0};
                            const uint8_t replace12[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0xFF, 0xD0};
                            KextPatch kext_patch12 {
                                {&kextList[i], find12, replace12, sizeof(find12), 2},
                                KernelVersion::Yosemite, KernelVersion::HighSierra
                            };
                            applyPatches(patcher, index, &kext_patch12, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset");

                            const uint8_t find13[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0x41, 0xFF, 0x55, 0x00};
                            const uint8_t replace13[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0x41, 0xFF, 0x55, 0x00};
                            KextPatch kext_patch13 {
                                {&kextList[i], find13, replace13, sizeof(find13), 2},
                                KernelVersion::Yosemite, KernelVersion::HighSierra
                            };
                            applyPatches(patcher, index, &kext_patch13, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset #2");
                        }
/*
                        const uint8_t find12[]    = {0xA2, 0x00, 0x00, 0xBA, 0x07, 0x00, 0x00, 0x00};
                        const uint8_t replace12[] = {0xA2, 0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x00};
                        KextPatch kext_patch12 {
                            {&kextList[i], find12, replace12, sizeof(find12), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch12, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Reset");

                        const uint8_t find13[]    = {0x83, 0x00, 0x00, 0xBA, 0x07, 0x00, 0x00, 0x00};
                        const uint8_t replace13[] = {0x83, 0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x00};
                        KextPatch kext_patch13 {
                            {&kextList[i], find13, replace13, sizeof(find13), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch13, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Reset #2");
*/
                        const uint8_t find14[]    = {0xBE, 0x88, 0x62, 0x01, 0x00};
                        const uint8_t replace14[] = {0xBE, 0x8C, 0x62, 0x01, 0x00};
                        KextPatch kext_patch14 {
                            {&kextList[i], find14, replace14, sizeof(find14), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch14, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300 #1");

                        const uint8_t find15[]    = {0xBE, 0x90, 0x62, 0x01, 0x00};
                        const uint8_t replace15[] = {0xBE, 0x94, 0x62, 0x01, 0x00};
                        KextPatch kext_patch15 {
                            {&kextList[i], find15, replace15, sizeof(find15), 4},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch15, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300 #2");

                        KextPatch kext_patch51 {
                            {&kextList[i], _ar9300_osprey_2p0_mac_core, _ar9565_osprey_2p0_mac_core, sizeof(_ar9300_osprey_2p0_mac_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch51, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_mac_core");

                        KextPatch kext_patch52 {
                            {&kextList[i], _ar9300_osprey_2p0_mac_postamble, _ar9565_osprey_2p0_mac_postamble, sizeof(_ar9300_osprey_2p0_mac_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch52, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_mac_postamble");

                        KextPatch kext_patch53 {
                            {&kextList[i], _ar9300_osprey_2p0_baseband_core, _ar9565_osprey_2p0_baseband_core, sizeof(_ar9300_osprey_2p0_baseband_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch53, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_baseband_core");

                        KextPatch kext_patch54 {
                            {&kextList[i], _ar9300_osprey_2p0_baseband_postamble, _ar9565_osprey_2p0_baseband_postamble, sizeof(_ar9300_osprey_2p0_baseband_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch54, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_baseband_postamble");

                        KextPatch kext_patch55 {
                            {&kextList[i], _ar9300_osprey_2p0_radio_core, _ar9565_osprey_2p0_radio_core, sizeof(_ar9300_osprey_2p0_radio_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch55, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_radio_core");

                        KextPatch kext_patch56 {
                            {&kextList[i], _ar9300_osprey_2p0_radio_postamble, _ar9565_osprey_2p0_radio_postamble, sizeof(_ar9300_osprey_2p0_radio_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch56, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_radio_postamble");

                        KextPatch kext_patch57 {
                            {&kextList[i], _ar9300_osprey_2p0_soc_preamble, _ar9565_osprey_2p0_soc_preamble, sizeof(_ar9300_osprey_2p0_soc_preamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch57, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_soc_preamble");

                        KextPatch kext_patch58 {
                            {&kextList[i], _ar9300_osprey_2p0_soc_postamble, _ar9565_osprey_2p0_soc_postamble, sizeof(_ar9300_osprey_2p0_soc_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch58, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_soc_postamble");

                        KextPatch kext_patch59 {
                            {&kextList[i], _ar9300Common_rx_gain_table_osprey_2p0, _ar9565Common_rx_gain_table_osprey_2p0, sizeof(_ar9300Common_rx_gain_table_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch59, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Common_rx_gain_table_osprey_2p0");

                        KextPatch kext_patch60 {
                            {&kextList[i], _ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0, _ar9565Modes_lowest_ob_db_tx_gain_table_osprey_2p0, sizeof(_ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch60, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0");

                        KextPatch kext_patch61 {
                            {&kextList[i], _ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0, _ar9565PciePhy_pll_on_clkreq_disable_L1_osprey_2p0, sizeof(_ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch61, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0");

                        KextPatch kext_patch62 {
                            {&kextList[i], _ar9300PciePhy_clkreq_enable_L1_osprey_2p0, _ar9565PciePhy_clkreq_enable_L1_osprey_2p0, sizeof(_ar9300PciePhy_clkreq_enable_L1_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch62, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300PciePhy_clkreq_enable_L1_osprey_2p0");

                        KextPatch kext_patch63 {
                            {&kextList[i], _ar9300Modes_fast_clock_osprey_2p0, _ar9565Modes_fast_clock_osprey_2p0, sizeof(_ar9300Modes_fast_clock_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch63, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Modes_fast_clock_osprey_2p0");
                    }
                    else if (PE_parse_boot_argn("-ath946x", tmp, sizeof(tmp)) ||
                             PE_parse_boot_argn("-ath9462", tmp, sizeof(tmp)) ||
                             PE_parse_boot_argn("-ath9463", tmp, sizeof(tmp))) {
                        const uint8_t find[]    = {0x39, 0x33, 0x38, 0x30};
                        const uint8_t replace[] = {0x39, 0x34, 0x36, 0x58};
                        KextPatch kext_patch {
                            {&kextList[i], find, replace, sizeof(find), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched AR946X");

                        const uint8_t find11[]    = {0x8D, 0x90, 0x00, 0x04, 0x00, 0x00};
                        const uint8_t replace11[] = {0x8D, 0x90, 0x00, 0x04, 0x08, 0x00};
                        KextPatch kext_patch11 {
                            {&kextList[i], find11, replace11, sizeof(find11), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch11, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300SetRxFilter");

                        if (getKernelVersion() == KernelVersion::Mavericks){
                            const uint8_t find12[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0xFF, 0xD0};
                            const uint8_t replace12[] = {0xBA, 0x03, 0x00, 0x00, 0x00, 0xFF, 0xD0};
                            KextPatch kext_patch12 {
                                {&kextList[i], find12, replace12, sizeof(find12), 2},
                                KernelVersion::Mavericks, KernelVersion::Mavericks
                            };
                            applyPatches(patcher, index, &kext_patch12, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset");

                            const uint8_t find13[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0xFF, 0x13};
                            const uint8_t replace13[] = {0xBA, 0x03, 0x00, 0x00, 0x00, 0xFF, 0x13};
                            KextPatch kext_patch13 {
                                {&kextList[i], find13, replace13, sizeof(find13), 2},
                                KernelVersion::Mavericks, KernelVersion::Mavericks
                            };
                            applyPatches(patcher, index, &kext_patch13, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset #2");
                        }
                        else {
                            const uint8_t find12[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0xFF, 0xD0};
                            const uint8_t replace12[] = {0xBA, 0x03, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0xFF, 0xD0};
                            KextPatch kext_patch12 {
                                {&kextList[i], find12, replace12, sizeof(find12), 2},
                                KernelVersion::Yosemite, KernelVersion::HighSierra
                            };
                            applyPatches(patcher, index, &kext_patch12, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset");

                            const uint8_t find13[]    = {0xBA, 0x07, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0x41, 0xFF, 0x55, 0x00};
                            const uint8_t replace13[] = {0xBA, 0x03, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE7, 0x41, 0xFF, 0x55, 0x00};
                            KextPatch kext_patch13 {
                                {&kextList[i], find13, replace13, sizeof(find13), 2},
                                KernelVersion::Yosemite, KernelVersion::HighSierra
                            };
                            applyPatches(patcher, index, &kext_patch13, 1);
                            progressState |= ProcessingState::AirPortAtheros40Patched;
                            DBGLOG("ath9k", " patched _ar9300Reset #2");
                        }
/*
                        const uint8_t find12[]    = {0xA2, 0x00, 0x00, 0xBA, 0x07, 0x00, 0x00, 0x00};
                        const uint8_t replace12[] = {0xA2, 0x00, 0x00, 0xBA, 0x03, 0x00, 0x00, 0x00};
                        KextPatch kext_patch12 {
                            {&kextList[i], find12, replace12, sizeof(find12), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch12, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Reset");

                        const uint8_t find13[]    = {0x83, 0x00, 0x00, 0xBA, 0x07, 0x00, 0x00, 0x00};
                        const uint8_t replace13[] = {0x83, 0x00, 0x00, 0xBA, 0x03, 0x00, 0x00, 0x00};
                        KextPatch kext_patch13 {
                            {&kextList[i], find13, replace13, sizeof(find13), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch13, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Reset #2");
*/
                        const uint8_t find14[]    = {0xBE, 0x88, 0x62, 0x01, 0x00};
                        const uint8_t replace14[] = {0xBE, 0x8C, 0x62, 0x01, 0x00};
                        KextPatch kext_patch14 {
                            {&kextList[i], find14, replace14, sizeof(find14), 2},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch14, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300 #1");

                        const uint8_t find15[]    = {0xBE, 0x90, 0x62, 0x01, 0x00};
                        const uint8_t replace15[] = {0xBE, 0x94, 0x62, 0x01, 0x00};
                        KextPatch kext_patch15 {
                            {&kextList[i], find15, replace15, sizeof(find15), 4},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch15, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300 #2");

                        KextPatch kext_patch51 {
                            {&kextList[i], _ar9300_osprey_2p0_mac_core, _ar946x_osprey_2p0_mac_core, sizeof(_ar9300_osprey_2p0_mac_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch51, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_mac_core");

                        KextPatch kext_patch52 {
                            {&kextList[i], _ar9300_osprey_2p0_mac_postamble, _ar946x_osprey_2p0_mac_postamble, sizeof(_ar9300_osprey_2p0_mac_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch52, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_mac_postamble");

                        KextPatch kext_patch53 {
                            {&kextList[i], _ar9300_osprey_2p0_baseband_core, _ar946x_osprey_2p0_baseband_core, sizeof(_ar9300_osprey_2p0_baseband_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch53, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_baseband_core");

                        KextPatch kext_patch54 {
                            {&kextList[i], _ar9300_osprey_2p0_baseband_postamble, _ar946x_osprey_2p0_baseband_postamble, sizeof(_ar9300_osprey_2p0_baseband_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch54, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_baseband_postamble");

                        KextPatch kext_patch55 {
                            {&kextList[i], _ar9300_osprey_2p0_radio_core, _ar946x_osprey_2p0_radio_core, sizeof(_ar9300_osprey_2p0_radio_core), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch55, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_radio_core");

                        KextPatch kext_patch56 {
                            {&kextList[i], _ar9300_osprey_2p0_radio_postamble, _ar946x_osprey_2p0_radio_postamble, sizeof(_ar9300_osprey_2p0_radio_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch56, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_radio_postamble");

                        KextPatch kext_patch57 {
                            {&kextList[i], _ar9300_osprey_2p0_soc_preamble, _ar946x_osprey_2p0_soc_preamble, sizeof(_ar9300_osprey_2p0_soc_preamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch57, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_soc_preamble");

                        KextPatch kext_patch58 {
                            {&kextList[i], _ar9300_osprey_2p0_soc_postamble, _ar946x_osprey_2p0_soc_postamble, sizeof(_ar9300_osprey_2p0_soc_postamble), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch58, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300_osprey_2p0_soc_postamble");

                        KextPatch kext_patch59 {
                            {&kextList[i], _ar9300Common_rx_gain_table_osprey_2p0, _ar946xCommon_rx_gain_table_osprey_2p0, sizeof(_ar9300Common_rx_gain_table_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch59, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Common_rx_gain_table_osprey_2p0");

                        KextPatch kext_patch60 {
                            {&kextList[i], _ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0, _ar946xModes_lowest_ob_db_tx_gain_table_osprey_2p0, sizeof(_ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch60, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Modes_lowest_ob_db_tx_gain_table_osprey_2p0");

                        KextPatch kext_patch61 {
                            {&kextList[i], _ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0, _ar946xPciePhy_pll_on_clkreq_disable_L1_osprey_2p0, sizeof(_ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch61, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300PciePhy_pll_on_clkreq_disable_L1_osprey_2p0");

                        KextPatch kext_patch62 {
                            {&kextList[i], _ar9300PciePhy_clkreq_enable_L1_osprey_2p0, _ar946xPciePhy_clkreq_enable_L1_osprey_2p0, sizeof(_ar9300PciePhy_clkreq_enable_L1_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch62, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300PciePhy_clkreq_enable_L1_osprey_2p0");

                        KextPatch kext_patch63 {
                            {&kextList[i], _ar9300Modes_fast_clock_osprey_2p0, _ar946xModes_fast_clock_osprey_2p0, sizeof(_ar9300Modes_fast_clock_osprey_2p0), 1},
                            KernelVersion::Mavericks, KernelVersion::Mojave
                        };
                        applyPatches(patcher, index, &kext_patch63, 1);
                        progressState |= ProcessingState::AirPortAtheros40Patched;
                        DBGLOG("ath9k", " patched _ar9300Modes_fast_clock_osprey_2p0");
                    }

                    if (progressState & ProcessingState::AirPortAtheros40Patched) {
                        SYSLOG("ath9k", "patching finished, scheduling post-patch IOKit rematch");
                        if (!gRematchCall)
                            gRematchCall = thread_call_allocate(rematchAfterPatch, this);
                        if (gRematchCall) {
                            uint64_t deadline = 0;
                            clock_interval_to_deadline(1, kSecondScale, &deadline);
                            thread_call_enter_delayed(gRematchCall, deadline);
                        }
                    }
                }
            }
        }
    }

    // Ignore all the errors for other processors
    patcher.clearError();
}

void ATH9K::applyPatches(KernelPatcher &patcher, size_t index, const KextPatch *patches, size_t patchNum) {
    DBGLOG("ath9k", " applying patches for %zu kext", index);
    for (size_t p = 0; p < patchNum; p++) {
        auto &patch = patches[p];
        if (patch.patch.kext->loadIndex == index) {
            if (patcher.compatibleKernel(patch.minKernel, patch.maxKernel)) {
                DBGLOG("ath9k", " applying %zu patch for %zu kext", p, index);
                patcher.applyLookupPatch(&patch.patch);
                if (patcher.getError() != KernelPatcher::Error::NoError) {
                    const uint8_t *f = patch.patch.find;
                    size_t sz = patch.patch.size;
                    DBGLOG("ath9k", "failed to apply patch %zu (size %zu, bytes %02X %02X %02X %02X) for kext %zu, error %d",
                           p, sz,
                           sz > 0 ? f[0] : 0, sz > 1 ? f[1] : 0, sz > 2 ? f[2] : 0, sz > 3 ? f[3] : 0,
                           index, patcher.getError());
                }
                patcher.clearError();
            }
        }
    }
}

