#include "platform/switch/SwitchExitCleanup.h"

#include <switch.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>

struct __pthread_t;

extern "C" int __real___syscall_thread_create(__pthread_t** thread, void* (*func)(void*),
	void* arg, void* stack_addr, size_t stack_size);

namespace
{
	struct StartInfo
	{
		void* (*func)(void*);
		void* arg;
	};

	struct ThreadRecord
	{
		Thread* thread;
		void* mirror;
		void* memory;
		bool running;
	};

	constexpr size_t kMaxThreads = 256;
	ThreadRecord s_threads[kMaxThreads];
	Mutex s_lock;
	std::atomic<bool> s_cleanedUp{false};

	int Register(Thread* self)
	{
		int slot = -1;
		mutexLock(&s_lock);
		for (size_t i = 0; i < kMaxThreads; ++i)
		{
			ThreadRecord& record = s_threads[i];
			// A finished thread whose mirror address came back was joined and closed.
			if (!record.running && record.mirror == self->stack_mirror)
				record = {};
			if (slot < 0 && !record.running && !record.mirror)
				slot = static_cast<int>(i);
		}
		if (slot >= 0)
			s_threads[slot] = {self, self->stack_mirror, self->stack_mem, true};
		mutexUnlock(&s_lock);
		return slot;
	}

	void Finish(int slot)
	{
		if (slot < 0)
			return;
		mutexLock(&s_lock);
		s_threads[slot].running = false;
		mutexUnlock(&s_lock);
	}

	void* ThreadEntry(void* argument)
	{
		const StartInfo start = *static_cast<StartInfo*>(argument);
		std::free(argument);
		Thread* self = threadGetSelf();
		const int slot = self ? Register(self) : -1;
		void* result = start.func(start.arg);
		Finish(slot);
		return result;
	}

	unsigned RunningCount()
	{
		unsigned count = 0;
		mutexLock(&s_lock);
		for (const ThreadRecord& record : s_threads)
			count += record.running ? 1 : 0;
		mutexUnlock(&s_lock);
		return count;
	}

	bool IsMappedStack(void* mirror, u64& size)
	{
		MemoryInfo info{};
		u32 pageInfo = 0;
		if (R_FAILED(svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(mirror))))
			return false;
		size = info.size;
		return (info.type & MemState_Type) == MemType_MappedMemory && info.addr == reinterpret_cast<u64>(mirror);
	}

	__attribute__((constructor(101))) void InitLock()
	{
		mutexInit(&s_lock);
	}
}

void SwitchExitCleanup_Run()
{
	if (s_cleanedUp.exchange(true))
		return;

	// Give threads that are already shutting down a moment to return.
	for (int waited = 0; waited < 500 && RunningCount() != 0; waited += 10)
		svcSleepThread(10'000'000);

	const Handle current = threadGetCurHandle();
	mutexLock(&s_lock);
	for (ThreadRecord& record : s_threads)
	{
		if (record.running && record.thread)
		{
			if (record.thread->handle == current)
				continue;
			// It must never run Cemu code again once the next program is loaded.
			svcSetThreadActivity(record.thread->handle, ThreadActivity_Paused);
			record.running = false;
		}
		if (!record.mirror)
			continue;
		u64 size = 0;
		// The unmap only succeeds while this record's mirror is still mapped
		// from its own stack, that is while the thread was never closed, so
		// its Thread is still valid. Clearing stack_mirror then tells
		// hbloader's own leftover-thread pass that the mirror is gone.
		if (IsMappedStack(record.mirror, size) &&
			R_SUCCEEDED(svcUnmapMemory(record.mirror, record.memory, size)) && record.thread)
			record.thread->stack_mirror = nullptr;
		record = {};
	}
	mutexUnlock(&s_lock);
}

extern "C" int __wrap___syscall_thread_create(__pthread_t** thread, void* (*func)(void*),
	void* arg, void* stack_addr, size_t stack_size);
extern "C" int __wrap___syscall_thread_create(__pthread_t** thread, void* (*func)(void*),
	void* arg, void* stack_addr, size_t stack_size)
{
	auto* start = static_cast<StartInfo*>(std::malloc(sizeof(StartInfo)));
	if (!start)
		return ENOMEM;
	*start = {func, arg};
	const int rc = __real___syscall_thread_create(thread, ThreadEntry, start, stack_addr, stack_size);
	if (rc != 0)
		std::free(start);
	return rc;
}
