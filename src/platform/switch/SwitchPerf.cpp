#include "platform/switch/SwitchPerf.h"

extern "C" {
#include <switch/arm/counter.h>
#include <switch/kernel/svc.h>
#include <switch/result.h>
}

#include <array>
#include <cstdio>

#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"

namespace
{
	constexpr size_t kCount = static_cast<size_t>(SwitchPerfCounter::Count);
	std::atomic_bool s_enabled{false};
	std::atomic_bool s_strictSync{false};
	std::array<std::atomic_uint64_t, kCount> s_counters{};

	uint64_t s_windowStart = 0;
	uint64_t s_threadTicksStart = 0;
	uint32_t s_pipelinesStart = 0;
	bool s_headerWritten = false;

	uint64_t ThreadTicks()
	{
		uint64_t ticks = 0;
		if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, -1)))
			ticks = 0;
		return ticks;
	}
}

void SwitchPerf_SetEnabled(bool enabled)
{
	s_enabled.store(enabled, std::memory_order_release);
}

bool SwitchPerf_Enabled()
{
	return s_enabled.load(std::memory_order_relaxed);
}

void SwitchPerf_SetStrictSync(bool strict)
{
	s_strictSync.store(strict, std::memory_order_release);
}

bool SwitchPerf_StrictSync()
{
	return s_strictSync.load(std::memory_order_relaxed);
}

void SwitchPerf_Add(SwitchPerfCounter counter, uint64_t value)
{
	s_counters[static_cast<size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

void SwitchPerf_OnPresent()
{
	SwitchPerf_Add(SwitchPerfCounter::Presents);
	const uint64_t now = armGetSystemTick();
	const uint64_t frequency = armGetSystemTickFreq();
	if (!SwitchPerf_Enabled())
	{
		s_windowStart = 0;
		return;
	}
	const uint32_t pipelines = performanceMonitor.vk.numGraphicPipelines.get();
	if (s_windowStart == 0)
	{
		for (auto& counter : s_counters)
			counter.store(0, std::memory_order_relaxed);
		s_windowStart = now;
		s_threadTicksStart = ThreadTicks();
		s_pipelinesStart = pipelines;
		return;
	}
	if (now - s_windowStart < frequency)
		return;

	std::array<uint64_t, kCount> values{};
	for (size_t i = 0; i < kCount; ++i)
		values[i] = s_counters[i].exchange(0, std::memory_order_relaxed);
	const double seconds = static_cast<double>(now - s_windowStart) / frequency;
	const uint64_t threadTicks = ThreadTicks();
	const double gpuThreadBusy = 100.0 * static_cast<double>(threadTicks - s_threadTicksStart) / (now - s_windowStart);
	auto perSecond = [&](SwitchPerfCounter counter) { return values[static_cast<size_t>(counter)] / seconds; };

	if (FILE* file = std::fopen("sdmc:/switch/cemu/perf-log.txt", "a"))
	{
		if (!s_headerWritten)
		{
			std::fprintf(file, "# per second: game fps, presented fps, draws, gpu thread busy %%, gpu wait ms, "
				"snapshot compared MB, copied MB, slow allocs, cache-full partial waits, cache-full drains, readback waits, new pipelines, gpu sync\n");
			s_headerWritten = true;
		}
		std::fprintf(file, "game %.1f  present %.1f  draws %.0f  gpu-thread %.0f%%  gpu-wait %.1fms  cmp %.1fMB  copy %.1fMB  "
			"slow-alloc %.0f  partial-wait %.0f  full-drain %.0f  readback-wait %.0f  pipelines %.0f  sync %s\n",
			perSecond(SwitchPerfCounter::GuestFrames), perSecond(SwitchPerfCounter::Presents),
			perSecond(SwitchPerfCounter::Draws), gpuThreadBusy,
			perSecond(SwitchPerfCounter::GpuWaitMicros) / 1000.0,
			perSecond(SwitchPerfCounter::SnapshotCompared) / (1024.0 * 1024.0),
			perSecond(SwitchPerfCounter::SnapshotCopied) / (1024.0 * 1024.0),
			perSecond(SwitchPerfCounter::SnapshotSlowAlloc), perSecond(SwitchPerfCounter::SnapshotPartialWait),
			perSecond(SwitchPerfCounter::SnapshotStall), perSecond(SwitchPerfCounter::ReadbackWait),
			(pipelines - s_pipelinesStart) / seconds, SwitchPerf_StrictSync() ? "strict" : "fast");
		std::fclose(file);
	}
	s_windowStart = now;
	s_threadTicksStart = threadTicks;
	s_pipelinesStart = pipelines;
}
