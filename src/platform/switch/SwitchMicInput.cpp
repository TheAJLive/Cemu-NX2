#include "audio/SwitchMicInputAPI.h"

extern "C" {
#include <switch/result.h>
#include <switch/services/audin.h>
}

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace
{
	constexpr size_t kBufferCount = 4;
	constexpr size_t kBufferSize = 0x1000;
	// About 100 ms at 32 kHz; older samples are dropped so the game never hears stale input.
	constexpr size_t kMaxQueued = 3200;

	std::mutex s_mutex;
	std::deque<sint16> s_samples;
	std::atomic<bool> s_running{false};
	std::thread s_thread;
	std::atomic<sint32> s_gainPercent{100};

	alignas(0x1000) u8 s_buffers[kBufferCount][kBufferSize];
	AudioInBuffer s_audioIn[kBufferCount];

	void LogDevices()
	{
		char names[0x100 * 8] = {};
		u32 count = 0;
		if (R_FAILED(audinListAudioIns(names, 8, &count)))
			return;
		for (u32 i = 0; i < count; ++i)
			cemuLog_log(LogType::Force, "Switch mic: input device {}: {}", i, &names[i * 0x100]);
	}

	void CaptureThread()
	{
		const u32 channels = std::max<u32>(1, audinGetChannelCount());
		const u32 rate = audinGetSampleRate() ? audinGetSampleRate() : 48000;
		cemuLog_log(LogType::Force, "Switch mic: capturing {} Hz, {} channel(s)", rate, channels);
		for (size_t i = 0; i < kBufferCount; ++i)
		{
			s_audioIn[i] = {};
			s_audioIn[i].buffer = s_buffers[i];
			s_audioIn[i].buffer_size = kBufferSize;
			s_audioIn[i].data_size = kBufferSize;
			audinAppendAudioInBuffer(&s_audioIn[i]);
		}
		if (R_FAILED(audinStartAudioIn()))
		{
			cemuLog_log(LogType::Force, "Switch mic: audinStartAudioIn failed");
			s_running = false;
			return;
		}
		// 48 kHz to 32 kHz: two output samples for every three input frames.
		uint32 phase = 0;
		bool loggedSignal = false;
		while (s_running)
		{
			AudioInBuffer* released = nullptr;
			u32 releasedCount = 0;
			if (R_FAILED(audinWaitCaptureFinish(&released, &releasedCount, 50'000'000ULL)) || !released)
				continue;
			const auto* frames = static_cast<const sint16*>(released->buffer);
			const size_t frameCount = released->data_size / (sizeof(sint16) * channels);
			const sint32 gain = s_gainPercent.load();
			{
				std::scoped_lock lock(s_mutex);
				for (size_t f = 0; f < frameCount; ++f)
				{
					sint32 mono = 0;
					for (u32 c = 0; c < channels; ++c)
						mono += frames[f * channels + c];
					mono = mono / (sint32)channels * gain / 100;
					if (!loggedSignal && std::abs(mono) > 2000)
					{
						loggedSignal = true;
						cemuLog_log(LogType::Force, "Switch mic: first loud sample ({})", mono);
					}
					if (rate == 48000)
					{
						phase += 2;
						if (phase < 3)
							continue;
						phase -= 3;
					}
					s_samples.push_back((sint16)std::clamp(mono, -32768, 32767));
				}
				while (s_samples.size() > kMaxQueued)
					s_samples.pop_front();
			}
			released->data_size = kBufferSize;
			audinAppendAudioInBuffer(released);
		}
		audinStopAudioIn();
	}
}

SwitchMicInputAPI::SwitchMicInputAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioInputAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
}

SwitchMicInputAPI::~SwitchMicInputAPI()
{
	Stop();
}

bool SwitchMicInputAPI::InitializeStatic()
{
	const Result rc = audinInitialize();
	if (R_FAILED(rc))
	{
		cemuLog_log(LogType::Force, "Switch mic: audinInitialize failed (0x{:08x})", (uint32)rc);
		return false;
	}
	LogDevices();
	return true;
}

std::vector<IAudioInputAPI::DeviceDescriptionPtr> SwitchMicInputAPI::GetDevices()
{
	return {std::make_shared<SwitchMicDeviceDescription>()};
}

bool SwitchMicInputAPI::ConsumeBlock(sint16* data)
{
	// The input volume setting defaults to 50, which is unity gain here.
	s_gainPercent = std::max<sint32>(0, GetVolume()) * 2;
	std::scoped_lock lock(s_mutex);
	for (uint32 i = 0; i < m_samplesPerBlock; ++i)
	{
		if (s_samples.empty())
		{
			std::memset(data + i, 0, (m_samplesPerBlock - i) * sizeof(sint16));
			break;
		}
		data[i] = s_samples.front();
		s_samples.pop_front();
	}
	return true;
}

bool SwitchMicInputAPI::Play()
{
	if (s_running.exchange(true))
		return true;
	s_thread = std::thread(CaptureThread);
	return true;
}

bool SwitchMicInputAPI::Stop()
{
	if (!s_running.exchange(false))
		return true;
	if (s_thread.joinable())
		s_thread.join();
	std::scoped_lock lock(s_mutex);
	s_samples.clear();
	return true;
}

bool SwitchMicInputAPI::IsPlaying() const
{
	return s_running;
}
