#include "platform/switch/SwitchThread.h"

#include <switch.h>

#include "Cemu/Logging/CemuLogging.h"

namespace
{
	constexpr int32_t kIdealCoreDontCare = -1;
	constexpr uint32_t kAppCoreMask = 0b0111u;
#if defined(__SWITCH2__)
	// The Switch 2 has eight cores. Which of them a game gets comes from its
	// process core mask, so helpers use every granted core above the three
	// Espresso cores.
	constexpr int kMaxCores = 8;
#else
	constexpr int kMaxCores = 4;
#endif
	constexpr uint32_t kAllCoreMask = (1u << kMaxCores) - 1;
	constexpr uint32_t kDefaultHelperCoreMask = 0b1000u;

	constexpr int32_t kBackgroundPriority = 63;

	uint32_t ProcessCoreMask()
	{
		uint64_t mask = 0;
		if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || (mask & kAllCoreMask) == 0)
			return kAllCoreMask;
		return static_cast<uint32_t>(mask & kAllCoreMask);
	}

	uint32_t HelperCoreMask()
	{
#if defined(__SWITCH2__)
		const uint32_t mask = ProcessCoreMask() & ~kAppCoreMask;
		return mask ? mask : kDefaultHelperCoreMask;
#else
		return kDefaultHelperCoreMask;
#endif
	}
} // namespace

void SwitchThread_RestoreProcessAffinity()
{
	uint64_t mask = 0;
	if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
	{
		cemuLog_log(LogType::Force, "Switch: could not read the process core mask");
		return;
	}
	const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, kIdealCoreDontCare, static_cast<u32>(mask));
	if (R_FAILED(rc))
		cemuLog_log(LogType::Force, "Switch: could not widen the main thread to core mask 0b{:b} (rc=0x{:08x})",
				    static_cast<u32>(mask), rc);
	else
		cemuLog_log(LogType::Force, "Switch: {} of {} CPU cores granted, main thread mask 0b{:b}",
				    SwitchThread_AllowedCoreCount(), kMaxCores, static_cast<u32>(mask));
}

int SwitchThread_AllowedCoreCount()
{
	uint64_t mask = 0;
	if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
		return 3;
	int cores = 0;
	for (int core = 0; core < kMaxCores; core++)
		if (mask & (UINT64_C(1) << core))
			cores++;
	return cores ? cores : 3;
}

int SwitchThread_HelperCoreCount()
{
	return __builtin_popcount(ProcessCoreMask() & ~kAppCoreMask);
}

uint32_t SwitchThread_WorkerCount(uint32_t minimum, uint32_t maximum)
{
	const uint32_t helpers = static_cast<uint32_t>(SwitchThread_HelperCoreCount());
	return helpers < minimum ? minimum : (helpers > maximum ? maximum : helpers);
}

void SwitchThread_PinToAppCores(const char* what)
{
	Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, kIdealCoreDontCare, kAppCoreMask);
	if (R_SUCCEEDED(rc))
		return;
	rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, kAppCoreMask);
	if (R_FAILED(rc))
		cemuLog_log(LogType::Force, "Switch: could not restrict {} to cores 0/1/2 (rc=0x{:08x})", what, rc);
}

#if defined(__SWITCH2__)
void SwitchThread_PinGpuThread()
{
	// New threads can run on any granted core, the PPC cores included. Give the
	// GPU thread the first helper core to itself.
	const uint32_t helpers = ProcessCoreMask() & ~kAppCoreMask;
	if (helpers == 0)
		return;
	const int32_t core = __builtin_ctz(helpers);
	const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
	if (R_SUCCEEDED(rc))
		cemuLog_log(LogType::Force, "Switch: GPU thread runs on core {}", core);
	else
		cemuLog_log(LogType::Force, "Switch: could not move the GPU thread to core {} (rc=0x{:08x})", core, rc);
}

void SwitchThread_UseHelperCores(const char* what)
{
	uint32_t helpers = ProcessCoreMask() & ~kAppCoreMask;
	// Leave the GPU thread's core alone when there are others to use.
	if (__builtin_popcount(helpers) > 1)
		helpers &= helpers - 1;
	if (helpers == 0)
		return;
	const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, kIdealCoreDontCare, helpers);
	if (R_FAILED(rc))
		cemuLog_log(LogType::Force, "Switch: could not move {} to core mask 0b{:b} (rc=0x{:08x})", what, helpers, rc);
}
#endif

bool SwitchThread_PinToHelperCore(const char* what, int32_t priority)
{
	const uint32_t helperMask = HelperCoreMask();
#if defined(__SWITCH2__)
	// The GPU thread takes the first helper core; small fixed workers like the
	// audio mixer go to the last one.
	const int32_t helperCore = 31 - __builtin_clz(helperMask);
#else
	const int32_t helperCore = __builtin_ctz(helperMask);
#endif
	const Result pinResult = svcSetThreadCoreMask(CUR_THREAD_HANDLE, helperCore, helperMask);
	if (R_SUCCEEDED(pinResult))
	{
		if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority))))
			cemuLog_log(LogType::Force, "Switch: {} runs on core {} at priority {}", what, helperCore, priority);
		else
			cemuLog_log(LogType::Force, "Switch: {} runs on core {} at the inherited priority", what, helperCore);
		return true;
	}

	SwitchThread_PinToAppCores(what);
	if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority))))
		cemuLog_log(LogType::Force, "Switch: could not raise {} to priority {}", what, priority);
	else
		cemuLog_log(LogType::Force, "Switch: {} stays on cores 0/1/2 at priority {} ({} of {} cores granted)",
					    what, priority, SwitchThread_AllowedCoreCount(), kMaxCores);
	return false;
}

void SwitchThread_AllowHelperCore(const char* what)
{
	if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, kIdealCoreDontCare, ProcessCoreMask())))
		SwitchThread_PinToAppCores(what);

	if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(kBackgroundPriority))))
		cemuLog_log(LogType::Force, "Switch: could not lower {} to background priority", what);
}
