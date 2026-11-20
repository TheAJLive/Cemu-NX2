#pragma once

#include <atomic>
#include <cstdint>

// Performance log: once a second, sdmc:/switch/cemu/perf-log.txt gets a line with these counters.
// Toggled in the quick menu. The counters cost an atomic add when the log is off.
enum class SwitchPerfCounter
{
	GuestFrames,        // TV scan buffer updates by the game
	Presents,           // host swapchain presents
	Draws,
	SnapshotCompared,   // bytes memcmp'd by the guest buffer cache
	SnapshotCopied,     // bytes copied into it
	SnapshotSlowAlloc,  // allocations that had to evict entries
	SnapshotStall,      // draws that waited for the GPU because the cache was full
	ReadbackWait,       // draws that waited for a texture readback
	GpuWaitMicros,      // time the GPU thread spent waiting for command buffers
	Count,
};

#if defined(__SWITCH__)
void SwitchPerf_SetEnabled(bool enabled);
bool SwitchPerf_Enabled();
void SwitchPerf_Add(SwitchPerfCounter counter, uint64_t value = 1);
// Call on the GPU thread once per present; writes the log line when a second has passed.
void SwitchPerf_OnPresent();
#else
inline void SwitchPerf_Add(SwitchPerfCounter, uint64_t = 1) {}
#endif
