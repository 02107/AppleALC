//
//  kern_patcher.cpp
//  KernelCommon
//
//  Copyright © 2016 vit9696. All rights reserved.
//

#include "kern_patcher_private.hpp"
#include "kern_patcher.hpp"

#include <mach/mach_types.h>

#ifdef KEXTPATCH_SUPPORT
static KernelPatcher *that {nullptr};
#endif /* KEXTPATCH_SUPPORT */
// Kernel version
extern const uint32_t version_major;

KernelPatcher::Error KernelPatcher::getError() {
	return code;
}

void KernelPatcher::clearError() {
	code = Error::NoError;
}

void KernelPatcher::init() {
	size_t id = loadKinfo("kernel", reinterpret_cast<const char **>(&kernelPaths), kernelPathsNum, true);
	
	if (getError() != Error::NoError || id != KernelID) {
		DBGLOG("patcher @ got %d error and %zu kernel id", getError(), id);
		return;
	}
	
	if (kinfos[KernelID]->getRunningAddresses() != KERN_SUCCESS) {
		DBGLOG("patcher @ failed to get running kernel mach info");
		code = Error::KernRunningInitFailure;
		return;
	}
}

void KernelPatcher::deinit() {
	// Deinitialise disassembler
	disasm.deinit();
	
	// Remove the patches
	if (kinfos.size() > 0) {
		if (kinfos[KernelID]->setKernelWriting(true) == KERN_SUCCESS) {
			for (size_t i = 0, n = kpatches.size(); i < n; i++) {
				kpatches[i]->restore();
			}
			kinfos[KernelID]->setKernelWriting(false);
		} else {
			SYSLOG("patcher @ failed to change kernel protection at patch removal");
		}
	}
	kpatches.deinit();
	
	// Deallocate kinfos
	kinfos.deinit();
	
	// Deallocate pages
	kpages.deinit();
}

size_t KernelPatcher::loadKinfo(const char *id, const char * const paths[], size_t num, bool isKernel) {
	auto info = MachInfo::create(isKernel);
	if (!info) {
		SYSLOG("patcher @ failed to allocate MachInfo for %s", id);
		code = Error::MemoryIssue;
	} else if (info->init(paths, num) != KERN_SUCCESS) {
		if ((isKernel && debugEnabled) || !isKernel)
			SYSLOG("patcher @ failed to init MachInfo for %s", id);
		code = Error::NoKinfoFound;
	} else if (!kinfos.push_back(info)) {
		SYSLOG("patcher @ unable to store loaded MachInfo for %s", id);
		code = Error::MemoryIssue;
	} else {
		return kinfos.last();
	}
	
	if (info) {
		info->deinit();
		MachInfo::deleter(info);
	}

	return INVALID;
}

#ifdef KEXTPATCH_SUPPORT
size_t KernelPatcher::loadKinfo(KernelPatcher::KextInfo *info) {
	if (!info) {
		SYSLOG("patcher @ loadKinfo got a null info");
		code = Error::MemoryIssue;
		return INVALID;
	}

	if (info->loadIndex != KernelPatcher::KextInfo::Unloaded) {
		DBGLOG("patcher @ provided KextInfo (%s) has already been loaded at %zu index", info->id, info->loadIndex);
		return info->loadIndex;
	}
	
	auto idx = loadKinfo(info->id, info->paths, info->pathNum);
	if (getError() == Error::NoError) {
		info->loadIndex = idx;
		DBGLOG("patcher @ loaded kinfo %s at %zu index", info->id, idx);
	}
	
	return idx;
}
#endif /* KEXTPATH_SUPPORT */

void KernelPatcher::updateRunningInfo(size_t id, mach_vm_address_t slide, size_t size) {
	if (id >= kinfos.size()) {
		SYSLOG("patcher @ invalid kinfo id %zu for running info update", id);
		return;
	}
	
	if (kinfos[id]->getRunningAddresses(slide, size) != KERN_SUCCESS) {
		SYSLOG("patcher @ failed to retrieve running info");
		code = Error::KernRunningInitFailure;
	}
}

bool KernelPatcher::compatibleKernel(uint32_t min, uint32_t max) {
	return (min == KernelAny || min <= version_major) &&
			(max == KernelAny || max >= version_major);
}

mach_vm_address_t KernelPatcher::solveSymbol(size_t id, const char *symbol) {
    if (id < kinfos.size()) {
        auto addr = kinfos[id]->solveSymbol(symbol);
        if (addr) {
            return addr;
        }
    } else {
        SYSLOG("patcher @ invalid kinfo id %zu for %s symbol lookup", id, symbol);
    }
    
    code = Error::NoSymbolFound;
    return 0;
}

#ifdef KEXTPATCH_SUPPORT
void KernelPatcher::setupKextListening() {
	// We have already done this
	if (that) return;
	
	// Lock primitives are needed to protect us from vm interrupts
	if (version_major >= 16) {
		usimpleLock = reinterpret_cast<void(*)(void *)>(solveSymbol(KernelID, "_usimple_lock"));
		usimpleUnlock = reinterpret_cast<void(*)(void *)>(solveSymbol(KernelID, "_usimple_unlock"));
		vmAllocationSitesLock = reinterpret_cast<void *>(solveSymbol(KernelID, "_vm_allocation_sites_lock"));
		
		if (!usimpleLock || !usimpleUnlock || !vmAllocationSitesLock) {
			SYSLOG("patcher @ unable to obtain locking primitives lock %d unlock %d vmlock %d",
				usimpleLock != nullptr, usimpleUnlock != nullptr, vmAllocationSitesLock != nullptr);
			usimpleLock = usimpleUnlock = nullptr;
			code = Error::NoSymbolFound;
			return;
		}
	}
	
	mach_vm_address_t s = solveSymbol(KernelID, "_OSKextLoadedKextSummariesUpdated");
	
	if (s) {
		DBGLOG("patcher @ _OSKextLoadedKextSummariesUpdated address %llX value %llX", s, *reinterpret_cast<uint64_t *>(s));
	} else {
		code = Error::NoSymbolFound;
		return;
	}
	
	loadedKextSummaries = reinterpret_cast<OSKextLoadedKextSummaryHeader **>(solveSymbol(KernelID, "_gLoadedKextSummaries"));

	if (loadedKextSummaries) {
		DBGLOG("patcher @ _gLoadedKextSummaries address %p", loadedKextSummaries);
	} else {
		code = Error::NoSymbolFound;
		return;
	}

	routeFunction(s, reinterpret_cast<mach_vm_address_t>(onKextSummariesUpdated));
	
	if (getError() == Error::NoError) {
		// Allow static functions to access the patcher body
		that = this;
	}
}

void KernelPatcher::waitOnKext(KextHandler *handler) {
	if (!that) {
		SYSLOG("patcher @ you should have called setupKextListening first");
		code = Error::KextListeningFailure;
		return;
	}
	
	if (!khandlers.push_back(handler)) {
		code = Error::MemoryIssue;
	}
}

void KernelPatcher::applyLookupPatch(const LookupPatch *patch) {
	if (!patch || !patch->kext || patch->kext->loadIndex == KextInfo::Unloaded) {
		SYSLOG("patcher @ an invalid lookup patch provided");
		code = Error::MemoryIssue;
		return;
	}
	
	uint8_t *off, *curr;
	size_t size;
	auto kinfo = kinfos[patch->kext->loadIndex];
	kinfo->getRunningPosition(off, size);
	
	curr = off;
	off += size - patch->size;
	size_t changes {0};
	for (size_t i = 0; curr < off && (i < patch->count || patch->count == 0); i++) {
		while (curr < off && memcmp(curr, patch->find, patch->size))
			curr++;
		
		if (curr != off) {
			if (kinfo->setKernelWriting(true) != KERN_SUCCESS) {
				SYSLOG("patcher @ lookup patching failed to write to kernel");
				code = Error::MemoryProtection;
				return;
			}
			for (size_t j = 0; j < patch->size; j++) {
				curr[j] = patch->replace[j];
			}
			if (kinfo->setKernelWriting(false) != KERN_SUCCESS) {
				SYSLOG("patcher @ lookup patching failed to disable kernel writing");
				code = Error::MemoryProtection;
				return;
			}
			changes++;
		}
	}
	
	if (changes != patch->count) {
		SYSLOG("patcher @ lookup patching applied only %zu patches out of %zu", changes, patch->count);
		code = Error::MemoryIssue;
	}
}
#endif /* KEXTPATCH_SUPPORT */

mach_vm_address_t KernelPatcher::routeFunction(mach_vm_address_t from, mach_vm_address_t to, bool buildWrapper, bool kernelRoute) {
	mach_vm_address_t diff = (to - (from + SmallJump));
	int32_t newArgument = static_cast<int32_t>(diff);
	
	DBGLOG("patcher @ diff %llX argument %X", diff, newArgument);
	
	bool absolute {false};
	
	if (diff != static_cast<mach_vm_address_t>(newArgument)) {
		DBGLOG("patcher @ will use absolute jumping to %llX", to);
		absolute = true;
		//SYSLOG("patcher @ cannot route %llX is too far from %llX", to, from);
		//code = Error::PointerRange;
		//return EINVAL;
	}
	
	mach_vm_address_t trampoline {0};
	
	if (buildWrapper) {
		trampoline = createTrampoline(from, absolute ? LongJump : SmallJump);
		if (!trampoline) return EINVAL;
	}
	
	Patch::All *opcode, *argument;
	if (absolute) {
		opcode = Patch::create<Patch::Variant::U64>(from, 0x0225FF);
		argument = Patch::create<Patch::Variant::U64>(from+sizeof(uint64_t), to);
	} else {
		opcode = Patch::create<Patch::Variant::U8>(from, 0xE9);
		argument = Patch::create<Patch::Variant::U32>(from+1, newArgument);
	}
	
	if (!opcode || !argument) {
		SYSLOG("patcher @ cannot create the necessary patches");
		code = Error::MemoryIssue;
		Patch::deleter(opcode); Patch::deleter(argument);
		return EINVAL;
	}
	
	if (kernelRoute && kinfos[KernelID]->setKernelWriting(true) != KERN_SUCCESS) {
		SYSLOG("patcher @ cannot change kernel memory protection");
		code = Error::MemoryProtection;
		Patch::deleter(opcode); Patch::deleter(argument);
		return EINVAL;
	}
	
	opcode->patch();
	argument->patch();

	if (kernelRoute) {
		kinfos[KernelID]->setKernelWriting(false);

		auto oidx = kpatches.push_back(opcode);
		auto aidx = kpatches.push_back(argument);

		if (!oidx || !aidx) {
			SYSLOG("patcher @ failed to store patches for later removal, you are in trouble");
			if (oidx) kpatches.erase(oidx);
			if (aidx) kpatches.erase(aidx);
			Patch::deleter(opcode); Patch::deleter(argument);
		}
	} else {
		Patch::deleter(opcode); Patch::deleter(argument);
	}

	return trampoline;
}

void KernelPatcher::tempExecutableMemory() {
	asm (".rept " xStringify(TempExecutableMemorySize) "\nnop\n.endr");
}

#ifdef KEXTPATCH_SUPPORT

void KernelPatcher::releaseMemoryLock() {
	if (that && that->wasAcquired && that->vmAllocationSitesLock && version_major >= KernelVersion::Sierra) {
		that->usimpleUnlock(that->vmAllocationSitesLock);
	}
}

void KernelPatcher::obtainMemoryLock() {
	if (that && that->wasAcquired && that->vmAllocationSitesLock && version_major >= KernelVersion::Sierra) {
		that->usimpleLock(that->vmAllocationSitesLock);
	}
}

#else

void KernelPatcher::releaseMemoryLock() {}

void KernelPatcher::obtainMemoryLock() {}

#endif /* KEXTPATCH_SUPPORT */

mach_vm_address_t KernelPatcher::createTrampoline(mach_vm_address_t func, size_t min) {
	if (!disasm.init()) {
		SYSLOG("patcher @ failed to use disasm");
		code = Error::DisasmFailure;
		return 0;
	}
	
	// Relative destination offset
	size_t off = disasm.instructionSize(func, min);
	
	if (!off || off > PAGE_SIZE - LongJump) {
		SYSLOG("patcher @ unsupported destination offset %zu", off);
		code = Error::DisasmFailure;
		return 0;
	}
	
	uint8_t *tempDataPtr = reinterpret_cast<uint8_t *>(tempExecutableMemory) + tempExecutableMemoryOff;
	
	tempExecutableMemoryOff += off + LongJump;
	
	if (tempExecutableMemoryOff >= TempExecutableMemorySize) {
		SYSLOG("patcher @ not enough executable memory requested %lld have %d", tempExecutableMemoryOff+1, TempExecutableMemorySize);
		code = Error::DisasmFailure;
	} else if (kinfos[KernelID]->setKernelWriting(true) == KERN_SUCCESS) {
		// Copy the prologue, assuming it is PIC
		memcpy(tempDataPtr, reinterpret_cast<void *>(func), off);
	
		// Add a jump
		routeFunction(reinterpret_cast<mach_vm_address_t>(tempDataPtr+off), func+off, false, false);
		
		kinfos[KernelID]->setKernelWriting(false);
		
		if (getError() == Error::NoError) {
			return reinterpret_cast<mach_vm_address_t>(tempDataPtr);
		} else {
			SYSLOG("patcher @ failed to route an inner trempoline");
		}
	} else {
		SYSLOG("patcher @ failed to set executable permissions");
		code = Error::MemoryProtection;
	}
	
	return 0;
}

#ifdef KEXTPATCH_SUPPORT
void KernelPatcher::onKextSummariesUpdated() {
	if (that) {
		// macOS 10.12 generates an interrupt during this call causing the boot to hang
		// unless we take the vm allocation lock
		if (version_major >= KernelVersion::Sierra) {
			that->usimpleLock(that->vmAllocationSitesLock);
			that->wasAcquired = true;
		}
		
		DBGLOG("patcher @ invoked at kext loading/unloading");
		
		if (that->khandlers.size() > 0 && that->loadedKextSummaries) {
			auto num = (*that->loadedKextSummaries)->numSummaries;
			if (num > 0) {
				OSKextLoadedKextSummary &last = (*that->loadedKextSummaries)->summaries[num-1];
				DBGLOG("patcher @ last kext is %llX and its name is %.*s", last.address, KMOD_MAX_NAME, last.name);
				// We may add khandlers items inside the handler
				for (size_t i = 0; i < that->khandlers.size(); i++) {
					if (!strncmp(that->khandlers[i]->id, last.name, KMOD_MAX_NAME)) {
						DBGLOG("patcher @ caught the right kext at %llX, invoking handler", last.address);
						that->khandlers[i]->address = last.address;
						that->khandlers[i]->size = last.size;
						that->khandlers[i]->handler(that->khandlers[i]);
						// Remove the item
						that->khandlers.erase(i);
						break;
					}
				}
			} else {
				SYSLOG("patcher @ no kext is currently loaded, this should not happen");
			}
		}
		
		//FIXME: we should not unlock this but instead avoid the lock in the caller
		//Fortunately there does not appear to be enough time for anybody to 
		if (version_major >= KernelVersion::Sierra) {
			that->usimpleUnlock(that->vmAllocationSitesLock);
			that->wasAcquired = false;
		}
	}
}
#endif /* KEXTPATCH_SUPPORT */