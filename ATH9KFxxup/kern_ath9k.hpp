//
//  kern_ath9k.hpp
//  ATH9KFxxup
//
//  Copyright © 2017 lvs1974. All rights reserved.
//
//  This kext is made based on lvs1974's NvidiaGraphicsFixup.
//

#ifndef kern_ath9k_hpp
#define kern_ath9k_hpp

#include <Headers/kern_patcher.hpp>
#include <IOKit/IOService.h>

class ATH9KDelayStub : public IOService {
    OSDeclareDefaultStructors(ATH9KDelayStub)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};

struct KextPatch {
    KernelPatcher::LookupPatch patch;
    uint32_t minKernel;
    uint32_t maxKernel;
};

class ATH9K {
public:
	bool init();
	void deinit();
	void resetProgress() {
        progressState = ProcessingState::NothingReady;
    }
	
private:
	/**
	 *  Patch kext if needed and prepare other patches
	 *
	 *  @param patcher KernelPatcher instance
	 *  @param index   kinfo handle
	 *  @param address kinfo load address
	 *  @param size    kinfo memory size
	 */
	void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
    
    /**
     *  Apply kext patches for loaded kext index
     *
     *  @param patcher    KernelPatcher instance
     *  @param index      kinfo index
     *  @param patches    patch list
     *  @param patchesNum patch number
     */
    void applyPatches(KernelPatcher &patcher, size_t index, const KextPatch *patches, size_t patchesNum);
	
	/**
	 *  Current progress mask
	 */
	struct ProcessingState {
		enum {
			NothingReady = 0,
			AirPortAtheros40Patched = 2,
			EverythingDone = AirPortAtheros40Patched,
		};
	};
    int progressState {ProcessingState::NothingReady};
};

#endif /* kern_ath9k_hpp */
