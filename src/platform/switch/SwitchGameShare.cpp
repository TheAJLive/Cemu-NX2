#include "platform/switch/SwitchGameShare.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

#include "Cemu/Logging/CemuLogging.h"
#include "WindowSystem.h"

// VulkanRenderer.cpp: the renderer's second (pad) swapchain on the guest's window.
bool VulkanRenderer_CreateGameShareSurface(int width, int height, std::string& error);
void VulkanRenderer_DestroyGameShareSurface();

namespace
{
constexpr const char* kErrorLogPath = "sdmc:/switch/cemu/gameshare-error.log";
constexpr const char* kLogPath = "sdmc:/switch/cemu/gameshare-log.txt";

std::atomic_bool s_requested{false};
std::atomic_bool s_swapRoles{false};
std::atomic_bool s_active{false};
std::atomic_bool s_separate{false};
std::mutex s_hostMutex;
FILE* s_log = nullptr;
std::mutex s_logMutex;
// pad the guest is using, for rumble
std::atomic<int> s_guestPadId{(int)HidNpadIdType_No1};
std::atomic<u32> s_guestPadStyle{0};
std::mutex s_nicknameMutex;
std::string s_guestNickname;

GameShareHost s_host{};
bool s_hostOpen = false;
NWindow s_guestWindow{};
bool s_guestWindowOpen = false;
bool s_guestSwapchain = false;
bool s_accountReady = false;
std::atomic<uint32_t> s_audioWritten{0};
std::atomic<uint32_t> s_audioDropped{0};

void OpenLog()
{
	std::scoped_lock lock(s_logMutex);
	if (!s_log)
		s_log = std::fopen(kLogPath, "w");
}

void Log(const char* format, ...)
{
	std::scoped_lock lock(s_logMutex);
	if (!s_log)
		return;
	va_list args;
	va_start(args, format);
	std::vfprintf(s_log, format, args);
	va_end(args);
	std::fputc('\n', s_log);
	std::fflush(s_log);
}

void WriteErrorLog(const std::string& text)
{
	cemuLog_log(LogType::Force, "GameShare: {}", text);
	Log("%s", text.c_str());
	if (FILE* file = std::fopen(kErrorLogPath, "w"))
	{
		std::fprintf(file, "%s\n", text.c_str());
		std::fclose(file);
	}
}

std::string DescribeFailure(const char* what, Result rc)
{
	char text[256];
	std::snprintf(text, sizeof(text), "%s failed (0x%08x, module %u desc %u)", what,
	              (unsigned)rc, (unsigned)R_MODULE(rc), (unsigned)R_DESCRIPTION(rc));
	return text;
}

bool GetHostUser(AccountUid& uid)
{
	if (!s_accountReady)
		s_accountReady = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));
	if (!s_accountReady)
		return false;
	uid = {};
	if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid))
		return true;
	uid = {};
	if (R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid))
		return true;
	AccountUid users[ACC_USER_LIST_SIZE]{};
	s32 count = 0;
	if (R_SUCCEEDED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &count)) && count > 0 &&
	    accountUidIsValid(&users[0]))
	{
		uid = users[0];
		return true;
	}
	return false;
}

// A window on the guest's display with the renderer's second swapchain on it. The game's GamePad view
// (TV view when swapped) is drawn there like on a second monitor.
bool OpenGuestScreen()
{
	Result rc = nwindowCreateForDisplay(&s_guestWindow, "player1");
	Log("open player1 window -> 0x%08x", (unsigned)rc);
	if (R_FAILED(rc))
		return false;
	s_guestWindowOpen = true;

	auto& canvas = WindowSystem::GetWindowInfo().canvas_pad;
	canvas.backend = WindowSystem::WindowHandleInfo::Backend::ViNN;
	canvas.display = nullptr;
	canvas.surface = &s_guestWindow;
	std::string error;
	if (VulkanRenderer_CreateGameShareSurface(kSwitchGameShareGuestWidth, kSwitchGameShareGuestHeight, error))
	{
		s_guestSwapchain = true;
		Log("player1 swapchain ready");
		return true;
	}
	Log("player1 swapchain failed: %s", error.c_str());
	VulkanRenderer_DestroyGameShareSurface();
	canvas.surface = nullptr;
	nwindowClose(&s_guestWindow);
	s_guestWindowOpen = false;
	return false;
}

// The swapchain itself goes away with the renderer.
void ForgetGuestSwapchain()
{
	WindowSystem::GetWindowInfo().canvas_pad.surface = nullptr;
	s_guestSwapchain = false;
}
} // namespace

void SwitchGameShare_Configure(bool requested, bool swapRoles)
{
	s_requested.store(requested, std::memory_order_release);
	s_swapRoles.store(swapRoles, std::memory_order_release);
}

void SwitchGameShare_Log(const char* text)
{
	OpenLog();
	Log("%s", text);
}

bool SwitchGameShare_Requested() { return s_requested.load(std::memory_order_acquire); }
bool SwitchGameShare_SwapRoles() { return s_swapRoles.load(std::memory_order_acquire); }
bool SwitchGameShare_Active() { return s_active.load(std::memory_order_acquire); }
bool SwitchGameShare_SeparateActive() { return s_separate.load(std::memory_order_acquire) && s_active.load(std::memory_order_acquire); }

bool SwitchGameShare_WriteGuestAudio(const int16_t* frames, uint32_t frameCount)
{
	if (!SwitchGameShare_SeparateActive())
		return false;
	std::scoped_lock lock(s_hostMutex);
	u32 freeFrames = 0;
	if (R_FAILED(stplGetGuestAudioFreeFrames(0, &freeFrames)))
		return true;
	if (freeFrames == 0)
	{
		++s_audioDropped;
		return true;
	}
	u32 written = 0;
	if (R_SUCCEEDED(stplWriteGuestAudio(0, frames, std::min(frameCount, freeFrames), &written)))
		s_audioWritten += written;
	return true;
}

SwitchGameShareResult SwitchGameShare_Begin(std::string& error)
{
	error.clear();
	if (s_hostOpen)
		return SwitchGameShareResult::Ok;
	OpenLog();

	AccountUid uid{};
	if (!GetHostUser(uid))
	{
		error = "no user account is available to host the session";
		WriteErrorLog(error);
		return SwitchGameShareResult::Failed;
	}

	// The guest screen is set up before matchmaking, so there is only ever one session: separate screens
	// when it works, a mirrored host screen when it doesn't.
	const bool separate = OpenGuestScreen();
	s_separate.store(separate, std::memory_order_release);
	Log("GameShare session, %s", separate ? "separate screens" : "mirrored screen");

	GameShareHostConfig config;
	gameshareHostConfigDefault(&config, 1);
	config.mode = separate ? StplStreamingMode_SeparateViews : StplStreamingMode_Mirror;
	config.audio = {true, 1.0f, 1.0f, 1.0f};
	config.user = uid;
	s_host = GameShareHost{};
	s_hostOpen = true;
	const Result rc = gameshareHostCreate(&s_host, &config);
	Log("gameshareHostCreate (mode %d) -> 0x%08x", (int)config.mode, (unsigned)rc);
	if (rc == GAMESHARE_RESULT_MATCHMAKING_CANCELLED)
	{
		SwitchGameShare_End();
		return SwitchGameShareResult::Cancelled;
	}
	if (R_FAILED(rc))
	{
		error = DescribeFailure("starting the session", rc);
		WriteErrorLog(error);
		SwitchGameShare_End();
		return SwitchGameShareResult::Failed;
	}

	char nickname[STPL_NICKNAME_LENGTH + 1] = {};
	if (R_SUCCEEDED(stplGetGuestNickname(0, nickname)) && nickname[0])
	{
		std::scoped_lock lock(s_nicknameMutex);
		s_guestNickname = nickname;
	}
	Log("stream started");
	s_active.store(true, std::memory_order_release);
	return SwitchGameShareResult::Ok;
}

void SwitchGameShare_End()
{
	s_active.store(false, std::memory_order_release);
	{
		std::scoped_lock lock(s_nicknameMutex);
		s_guestNickname.clear();
	}
	s_guestPadStyle.store(0, std::memory_order_relaxed);
	if (!s_hostOpen)
		return;
	Result rc;
	{
		std::scoped_lock lock(s_hostMutex);
		rc = gameshareHostClose(&s_host);
	}
	Log("session closed -> 0x%08x (guest audio written %u, dropped %u)", (unsigned)rc, s_audioWritten.load(),
	    s_audioDropped.load());
	if (R_FAILED(rc))
		WriteErrorLog(DescribeFailure("closing the session", rc));
	// A failed close keeps the work memory; the process exit releases it.
	s_hostOpen = false;
}

void SwitchGameShare_ReleaseGuestScreen()
{
	ForgetGuestSwapchain();
	s_separate.store(false, std::memory_order_release);
	if (!s_guestWindowOpen)
		return;
	nwindowClose(&s_guestWindow);
	s_guestWindowOpen = false;
	Log("closed player1 window");
}

bool SwitchGameShare_GetGuestInput(SwitchGameShareInput& out)
{
	if (!SwitchGameShare_Active())
		return false;
	out = {};
	bool any = false;
	for (const HidNpadIdType id : {HidNpadIdType_No1, HidNpadIdType_Handheld, HidNpadIdType_No2})
	{
		StplGuestNpadState pad{};
		const Result rc = stplGetGuestNpadState(0, id, &pad);
		if (R_FAILED(rc) || !pad.style || !(pad.state.attributes & HidNpadAttribute_IsConnected))
			continue;
		any = true;
		s_guestPadId.store((int)id, std::memory_order_relaxed);
		s_guestPadStyle.store(pad.style, std::memory_order_relaxed);
		out.buttons |= pad.state.buttons;
		const int32_t values[4] = {pad.state.analog_stick_l.x, pad.state.analog_stick_l.y,
		                           pad.state.analog_stick_r.x, pad.state.analog_stick_r.y};
		for (int i = 0; i < 4; ++i)
			if (std::abs(values[i]) > std::abs(out.stick[i]))
				out.stick[i] = values[i];
	}

	HidTouchScreenState touch{};
	if (R_SUCCEEDED(stplGetGuestTouchScreenState(0, &touch)) && touch.count > 0)
	{
		out.touchDown = true;
		out.touchX = (int32_t)touch.touches[0].x;
		out.touchY = (int32_t)touch.touches[0].y;
		any = true;
	}

	// gyro: try the sensor that matches the guest's controller, remember the one that answers
	struct SensorProbe { HidNpadIdType pad; StplSixAxisSensor sensor; };
	static constexpr SensorProbe kProbes[] = {
		{HidNpadIdType_Handheld, StplSixAxisSensor_Handheld},
		{HidNpadIdType_No1, StplSixAxisSensor_FullKey},
		{HidNpadIdType_No1, StplSixAxisSensor_JoyDualLeft},
		{HidNpadIdType_No1, StplSixAxisSensor_JoyDualRight},
		{HidNpadIdType_No1, StplSixAxisSensor_JoyLeft},
		{HidNpadIdType_No1, StplSixAxisSensor_JoyRight},
		{HidNpadIdType_No2, StplSixAxisSensor_FullKey},
	};
	static int s_probe = 0;
	for (int attempt = 0; attempt < (int)(sizeof(kProbes) / sizeof(kProbes[0])); ++attempt)
	{
		const int index = (s_probe + attempt) % (int)(sizeof(kProbes) / sizeof(kProbes[0]));
		HidSixAxisSensorState state{};
		const Result rc = stplGetGuestSixAxisSensorState(0, kProbes[index].pad, kProbes[index].sensor, &state);
		if (R_FAILED(rc) || !(state.attributes & HidSixAxisSensorAttribute_IsConnected) ||
		    state.sampling_number == 0)
			continue;
		s_probe = index;
		out.hasMotion = true;
		out.motionSampling = state.sampling_number;
		out.gyro[0] = state.angular_velocity.x;
		out.gyro[1] = state.angular_velocity.y;
		out.gyro[2] = state.angular_velocity.z;
		out.accel[0] = state.acceleration.x;
		out.accel[1] = state.acceleration.y;
		out.accel[2] = state.acceleration.z;
		any = true;
		break;
	}
	return any;
}

void SwitchGameShare_SetGuestRumble(float amplitude)
{
	const u32 styles = s_guestPadStyle.load(std::memory_order_relaxed);
	if (!SwitchGameShare_Active() || styles == 0)
		return;
	const u32 style = styles & (~styles + 1);
	StplVibrationDeviceHandle handles[2];
	s32 count = 0;
	std::scoped_lock lock(s_hostMutex);
	if (R_FAILED(stplGetGuestVibrationDeviceHandles(handles, 2, &count, 0, (HidNpadIdType)s_guestPadId.load(), style)))
		return;
	const HidVibrationValue value{amplitude, 160.0f, amplitude, 320.0f};
	for (s32 i = 0; i < count; ++i)
		stplSendGuestVibrationValue(handles[i], &value);
}

std::string SwitchGameShare_GetGuestNickname()
{
	std::scoped_lock lock(s_nicknameMutex);
	return s_guestNickname;
}
