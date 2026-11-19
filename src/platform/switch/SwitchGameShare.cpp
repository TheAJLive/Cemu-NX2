#include "platform/switch/SwitchGameShare.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/CemuConfig.h"
#include "imgui/imgui_extension.h"
#include "platform/switch/SwitchPlatform.h"

namespace
{
constexpr Result kMatchmakeCancelled = MAKERESULT(287, 30);
constexpr const char* kErrorLogPath = "sdmc:/switch/cemu/gameshare-error.log";
constexpr const char* kLogPath = "sdmc:/switch/cemu/gameshare-log.txt";
constexpr u32 kGuestWidth = 1280;
constexpr u32 kGuestHeight = 720;
constexpr s32 kGuestAudioMaxFrames = 8192;

std::atomic_bool s_lobbyRequested{false};
std::atomic_bool s_swapRoles{false};
std::atomic_bool s_active{false};
std::atomic_bool s_separate{true};
std::mutex s_hostMutex;
FILE* s_log = nullptr;
std::mutex s_logMutex;
std::atomic_bool s_lobbyActive{false};
// pad the guest is using, for rumble
std::atomic<int> s_guestPadId{(int)HidNpadIdType_No1};
std::atomic<u32> s_guestPadStyle{0};
std::mutex s_nicknameMutex;
std::string s_guestNickname;

GameShareLocalHost s_host{};
GameShareLocalGuestDisplay* s_display = nullptr;
std::mutex s_frameMutex;
std::condition_variable s_frameCv;
std::vector<uint32_t> s_frame;
bool s_frameReady = false;
bool s_presenterStop = false;
std::thread s_presenter;
std::atomic<uint64_t> s_lastGameFrameMs{0};
std::atomic<uint32_t> s_framesSubmitted{0};
std::atomic<uint32_t> s_framesPresented{0};
std::atomic<uint32_t> s_audioWritten{0};
std::atomic<uint32_t> s_audioDropped{0};
std::atomic<s32> s_audioFilled{0};
bool s_hostOpen = false;
bool s_accountReady = false;

void WriteErrorLog(const std::string& text)
{
	cemuLog_log(LogType::Force, "GameShare: {}", text);
	if (FILE* file = std::fopen(kErrorLogPath, "w"))
	{
		std::fprintf(file, "%s\n", text.c_str());
		std::fclose(file);
	}
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

uint64_t NowMs()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::steady_clock::now().time_since_epoch()).count();
}

void FillRect(std::vector<uint32_t>& pixels, int x, int y, int w, int h, uint32_t color)
{
	for (int row = std::max(y, 0); row < std::min<int>(y + h, kGuestHeight); ++row)
		for (int col = std::max(x, 0); col < std::min<int>(x + w, kGuestWidth); ++col)
			pixels[(size_t)row * kGuestWidth + col] = color;
}

// Draws with libnx's built-in console font.
void DrawText(std::vector<uint32_t>& pixels, int x, int y, const std::string& text, int scale, uint32_t color)
{
	const ConsoleFont* font = &consoleGetDefault()->font;
	const u16* glyphs = static_cast<const u16*>(font->gfx);
	for (const char ch : text)
	{
		if (x + 16 * scale > (int)kGuestWidth)
			break;
		unsigned char c = (unsigned char)ch;
		if (c < 32 || c > 126)
			c = '?';
		const u16* glyph = glyphs + (c - font->asciiOffset) * 16;
		for (int row = 0; row < 16; ++row)
			for (int col = 0; col < 16; ++col)
				if (glyph[row] & (0x8000u >> col))
					FillRect(pixels, x + col * scale, y + row * scale, scale, scale, color);
		x += 16 * scale;
	}
}

std::string DescribeFailure(const char* what, Result rc)
{
	char text[256];
	std::snprintf(text, sizeof(text), "%s failed (0x%08x, module %u desc %u) during '%s'", what,
	              (unsigned)rc, (unsigned)R_MODULE(rc), (unsigned)R_DESCRIPTION(rc),
	              s_host.operation ? s_host.operation : "?");
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

// ---- lobby state ----
struct LobbyGame
{
	std::string name;
	uint64_t titleId;
};

std::mutex s_lobbyMutex;
std::vector<LobbyGame> s_games;
int s_selection = 0; // 0 = "Swap Host and Guest", 1.. = games
bool s_scanFinished = false;
std::chrono::steady_clock::time_point s_lastListBuild;
u64 s_previousButtons = 0;
PadState s_hostPad;
bool s_hostPadReady = false;
std::string s_status;
u32 s_guestFlags = 0;
std::chrono::steady_clock::time_point s_lastConnectionPoll;

void BuildGameList()
{
	std::vector<LobbyGame> games;
	for (TitleId id : CafeTitleList::GetAllTitleIds())
	{
		TitleIdParser parser(id);
		if (parser.GetType() != TitleIdParser::TITLE_TYPE::BASE_TITLE)
			continue;
		TitleInfo info;
		if (!CafeTitleList::GetFirstByTitleId(id, info))
			continue;
		std::string name = info.GetMetaTitleName();
		if (name.empty())
			name = fmt::format("{:016x}", (uint64_t)id);
		games.push_back({std::move(name), (uint64_t)id});
	}
	std::sort(games.begin(), games.end(), [](const LobbyGame& a, const LobbyGame& b) { return a.name < b.name; });
	games.erase(std::unique(games.begin(), games.end(),
	                        [](const LobbyGame& a, const LobbyGame& b) { return a.titleId == b.titleId; }),
	            games.end());
	std::scoped_lock lock(s_lobbyMutex);
	s_games = std::move(games);
	const int maximum = (int)s_games.size();
	s_selection = std::clamp(s_selection, 0, maximum);
}

// What the guest sees until a game is running.
void PaintLobbyFrame(std::vector<uint32_t>& pixels, const char* heading)
{
	pixels.assign((size_t)kGuestWidth * kGuestHeight, RGBA8_MAXALPHA(8, 14, 26));
	FillRect(pixels, 0, 0, kGuestWidth, 8, RGBA8_MAXALPHA(94, 201, 255));
	DrawText(pixels, 60, 120, "GamePad GameShare", 4, RGBA8_MAXALPHA(245, 245, 245));
	DrawText(pixels, 60, 230, heading, 2, RGBA8_MAXALPHA(94, 201, 255));
	std::string selected;
	{
		std::scoped_lock lock(s_lobbyMutex);
		if (s_selection == 0)
			selected = "Swap Host and Guest";
		else if (s_selection - 1 < (int)s_games.size())
			selected = s_games[(size_t)s_selection - 1].name;
		DrawText(pixels, 60, 330, s_status, 2, RGBA8_MAXALPHA(200, 200, 200));
	}
	if (!selected.empty())
		DrawText(pixels, 60, 420, "> " + selected, 2, RGBA8_MAXALPHA(245, 245, 245));
}

void PresenterMain()
{
	std::vector<uint32_t> local;
	uint64_t lastStats = NowMs();
	uint32_t lastPresented = 0;
	for (;;)
	{
		bool fromGame = false;
		{
			std::unique_lock lock(s_frameMutex);
			s_frameCv.wait_for(lock, std::chrono::milliseconds(100), [] { return s_frameReady || s_presenterStop; });
			if (s_presenterStop)
				return;
			if (s_frameReady)
			{
				local.swap(s_frame);
				s_frameReady = false;
				fromGame = true;
			}
		}
		if (!fromGame)
		{
			// No game frames lately: show the lobby.
			if (NowMs() - s_lastGameFrameMs.load() < 500)
				continue;
			PaintLobbyFrame(local, SwitchGameShare_LobbyActive() ? "Host is choosing a game" : "Starting...");
		}
		if (local.size() != (size_t)kGuestWidth * kGuestHeight)
			continue;
		const Result rc = gameshareLocalGuestDisplayPresent(s_display, local.data(), kGuestWidth);
		if (R_SUCCEEDED(rc))
			++s_framesPresented;
		else if (R_VALUE(rc) != KERNELRESULT(TimedOut))
			Log("present failed 0x%08x", (unsigned)rc);
		const uint64_t now = NowMs();
		if (now - lastStats >= 5000)
		{
			const uint32_t presented = s_framesPresented.load();
			Log("guest display %.1f fps (submitted %u), audio written %u dropped %u filled %d",
			    (presented - lastPresented) * 1000.0 / (double)(now - lastStats), s_framesSubmitted.load(),
			    s_audioWritten.load(), s_audioDropped.load(), (int)s_audioFilled.load());
			lastPresented = presented;
			lastStats = now;
		}
	}
}

void StopPresenter()
{
	if (!s_presenter.joinable())
		return;
	{
		std::scoped_lock lock(s_frameMutex);
		s_presenterStop = true;
	}
	s_frameCv.notify_all();
	s_presenter.join();
}
} // namespace

void SwitchGameShare_Configure(bool lobbyRequested, bool swapRoles)
{
	s_lobbyRequested.store(lobbyRequested, std::memory_order_release);
	s_swapRoles.store(swapRoles, std::memory_order_release);
	s_separate.store(true, std::memory_order_release);
}

bool SwitchGameShare_SeparateActive() { return s_separate.load(std::memory_order_acquire) && s_active.load(std::memory_order_acquire); }

bool SwitchGameShare_WantsGuestFrame(bool padView)
{
	return SwitchGameShare_SeparateActive() && (padView != s_swapRoles.load(std::memory_order_acquire));
}

void SwitchGameShare_SubmitGuestFrame(const uint32_t* rgba)
{
	{
		std::scoped_lock lock(s_frameMutex);
		s_frame.assign(rgba, rgba + (size_t)kGuestWidth * kGuestHeight);
		s_frameReady = true;
	}
	s_lastGameFrameMs.store(NowMs());
	++s_framesSubmitted;
	s_frameCv.notify_one();
}

bool SwitchGameShare_WriteGuestAudio(const int16_t* frames, uint32_t frameCount)
{
	if (!SwitchGameShare_SeparateActive())
		return false;
	std::scoped_lock lock(s_hostMutex);
	s32 filled = 0;
	if (R_FAILED(gameshareLocalHostGetGuestAudioFilled(&s_host, 0, &filled)))
		return true;
	s_audioFilled.store(filled);
	if (filled > kGuestAudioMaxFrames)
	{
		++s_audioDropped;
		return true;
	}
	s32 written = 0;
	if (R_SUCCEEDED(gameshareLocalHostWriteGuestAudio(&s_host, 0, frames, (s32)frameCount, &written)))
		s_audioWritten += (uint32_t)std::max<s32>(written, 0);
	return true;
}

bool SwitchGameShare_LobbyRequested() { return s_lobbyRequested.load(std::memory_order_acquire); }
bool SwitchGameShare_SwapRoles() { return s_swapRoles.load(std::memory_order_acquire); }
bool SwitchGameShare_Active() { return s_active.load(std::memory_order_acquire); }
bool SwitchGameShare_LobbyActive() { return s_lobbyActive.load(std::memory_order_acquire); }
void SwitchGameShare_LobbySetActive(bool active) { s_lobbyActive.store(active, std::memory_order_release); }

SwitchGameShareResult SwitchGameShare_Begin(std::string& error)
{
	error.clear();
	if (s_hostOpen)
		return SwitchGameShareResult::Ok;
	s_host = GameShareLocalHost{};
	{
		std::scoped_lock lock(s_logMutex);
		if (!s_log)
			s_log = std::fopen(kLogPath, "w");
	}
	const bool separate = s_separate.load();
	Log("GameShare session, %s", separate ? "separate screens" : "mirrored screen");
	GameShareLocalStage stage = GameShareLocalStage_Lookup;
	Result rc = gameshareLocalOpen(&s_host.client, &stage);
	if (R_FAILED(rc))
	{
		error = DescribeFailure("opening stpl:u", rc) + " (stage " + std::to_string((int)stage) + ")";
		WriteErrorLog(error);
		return SwitchGameShareResult::Failed;
	}
	s_hostOpen = true;

	const GameShareLocalAudio audio{true, 1.0f, 1.0f, 1.0f};
	rc = gameshareLocalHostPrepareEx(&s_host, 1, &audio, separate ? 1 : 0);
	if (R_FAILED(rc))
	{
		error = DescribeFailure("preparing the session", rc);
		WriteErrorLog(error);
		SwitchGameShare_End();
		return SwitchGameShareResult::Failed;
	}
	// Guest touch and gyro; must be enabled after prepare and before matchmaking.
	rc = gameshareLocalHostEnableSensors(&s_host, 1280, 720);
	if (R_FAILED(rc))
	{
		error = DescribeFailure("enabling guest touch and gyro", rc);
		WriteErrorLog(error);
		SwitchGameShare_End();
		return SwitchGameShareResult::Failed;
	}

	AccountUid uid{};
	if (!GetHostUser(uid))
	{
		error = "no user account is available to host the session";
		WriteErrorLog(error);
		SwitchGameShare_End();
		return SwitchGameShareResult::Failed;
	}

	if (separate)
	{
		// The guest display has to exist before matchmaking.
		rc = gameshareLocalGuestDisplayOpen(&s_display, 0, kGuestWidth, kGuestHeight);
		Log("open guest display (%s) -> 0x%08x", gameshareLocalGuestDisplayLastStep(), (unsigned)rc);
		if (R_FAILED(rc))
		{
			s_display = nullptr;
			WriteErrorLog(DescribeFailure("opening the guest display", rc));
			SwitchGameShare_End();
			Log("falling back to a mirrored screen");
			s_separate.store(false, std::memory_order_release);
			return SwitchGameShare_Begin(error);
		}
	}

	rc = gameshareLocalHostMatchmake(&s_host, uid);
	if (rc == kMatchmakeCancelled)
	{
		SwitchGameShare_End();
		return SwitchGameShareResult::Cancelled;
	}
	if (R_FAILED(rc))
	{
		error = DescribeFailure("matchmaking", rc);
		WriteErrorLog(error);
		SwitchGameShare_End();
		return SwitchGameShareResult::Failed;
	}

	if (separate)
	{
		// First frame goes out before the stream starts.
		std::vector<uint32_t> first;
		PaintLobbyFrame(first, "Starting...");
		rc = gameshareLocalGuestDisplayPresent(s_display, first.data(), kGuestWidth);
		Log("initial guest frame -> 0x%08x", (unsigned)rc);
		if (R_FAILED(rc) && R_VALUE(rc) != KERNELRESULT(TimedOut))
		{
			error = DescribeFailure("presenting the first guest frame", rc);
			WriteErrorLog(error);
			SwitchGameShare_End();
			return SwitchGameShareResult::Failed;
		}
	}

	rc = gameshareLocalHostStart(&s_host);
	if (R_FAILED(rc))
	{
		error = DescribeFailure("starting the stream", rc);
		WriteErrorLog(error);
		SwitchGameShare_End();
		return SwitchGameShareResult::Failed;
	}
	Log("stream started");
	s_active.store(true, std::memory_order_release);
	if (separate)
	{
		s_presenterStop = false;
		s_frameReady = false;
		s_lastGameFrameMs.store(0);
		s_presenter = std::thread(PresenterMain);
	}
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
	StopPresenter();
	if (!s_hostOpen)
		return;
	Result rc;
	{
		std::scoped_lock lock(s_hostMutex);
		rc = gameshareLocalHostClose(&s_host);
	}
	if (s_display)
	{
		const Result displayRc = gameshareLocalGuestDisplayClose(s_display);
		Log("close guest display -> 0x%08x", (unsigned)displayRc);
		if (R_SUCCEEDED(displayRc))
			s_display = nullptr;
	}
	Log("session closed -> 0x%08x", (unsigned)rc);
	if (R_FAILED(rc))
		WriteErrorLog(DescribeFailure("closing the session", rc));
	else
		s_hostOpen = false;
	// A failed close keeps the work memory; the process exit releases it.
	s_hostOpen = false;
}

bool SwitchGameShare_GetGuestInput(SwitchGameShareInput& out)
{
	if (!SwitchGameShare_Active())
		return false;
	out = {};
	bool any = false;
	for (const HidNpadIdType id : {HidNpadIdType_No1, HidNpadIdType_Handheld, HidNpadIdType_No2})
	{
		GameShareLocalPad pad{};
		const Result rc = gameshareLocalReadGuestPad(&s_host.client, 0, id, &pad);
		if (R_FAILED(rc) || !pad.connected)
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

	// touch
	HidTouchScreenState touch{};
	if (R_SUCCEEDED(gameshareLocalReadGuestTouch(&s_host.client, 0, &touch)) && touch.count > 0)
	{
		out.touchDown = true;
		out.touchX = (int32_t)touch.touches[0].x;
		out.touchY = (int32_t)touch.touches[0].y;
		any = true;
	}

	// gyro: try the sensor that matches the guest's controller, remember the one that answers
	struct SensorProbe { HidNpadIdType pad; GameShareLocalSensor sensor; };
	static constexpr SensorProbe kProbes[] = {
		{HidNpadIdType_Handheld, GameShareLocalSensor_Handheld},
		{HidNpadIdType_No1, GameShareLocalSensor_FullKey},
		{HidNpadIdType_No1, GameShareLocalSensor_JoyDualLeft},
		{HidNpadIdType_No1, GameShareLocalSensor_JoyDualRight},
		{HidNpadIdType_No1, GameShareLocalSensor_JoyLeft},
		{HidNpadIdType_No1, GameShareLocalSensor_JoyRight},
		{HidNpadIdType_No2, GameShareLocalSensor_FullKey},
	};
	static int s_probe = 0;
	for (int attempt = 0; attempt < (int)(sizeof(kProbes) / sizeof(kProbes[0])); ++attempt)
	{
		const int index = (s_probe + attempt) % (int)(sizeof(kProbes) / sizeof(kProbes[0]));
		HidSixAxisSensorState state{};
		const Result rc = gameshareLocalReadGuestSixAxis(&s_host.client, 0, kProbes[index].pad,
		                                                 kProbes[index].sensor, &state);
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
	GameShareLocalVibrationHandle handles[2];
	std::scoped_lock lock(s_hostMutex);
	const s32 count = gameshareLocalMakeVibrationHandles(handles, 2, 0, (HidNpadIdType)s_guestPadId.load(), style);
	const HidVibrationValue value{amplitude, 160.0f, amplitude, 320.0f};
	for (s32 i = 0; i < count; ++i)
		gameshareLocalHostSendVibration(&s_host, handles[i], &value);
}

std::string SwitchGameShare_GetGuestNickname()
{
	std::scoped_lock lock(s_nicknameMutex);
	return s_guestNickname;
}

void SwitchGameShare_LobbyBegin()
{
	if (!s_hostPadReady)
	{
		padInitializeAny(&s_hostPad);
		s_hostPadReady = true;
	}
	CafeTitleList::ClearScanPaths();
	for (const std::string& path : GetConfig().game_paths)
		CafeTitleList::AddScanPath(_utf8ToPath(path));
	CafeTitleList::Refresh();
	s_scanFinished = false;
	s_lastListBuild = {};
	s_previousButtons = 0;
	s_status = "Waiting for the guest...";
	BuildGameList();
}

bool SwitchGameShare_LobbyUpdate(SwitchGameShareLobbyChoice& choice)
{
	const auto now = std::chrono::steady_clock::now();
	if (!s_scanFinished && now - s_lastListBuild > std::chrono::seconds(1))
	{
		s_lastListBuild = now;
		s_scanFinished = !CafeTitleList::IsScanning();
		BuildGameList();
	}
	if (now - s_lastConnectionPoll > std::chrono::milliseconds(500))
	{
		s_lastConnectionPoll = now;
		GameShareLocalConnection connection{};
		std::scoped_lock hostLock(s_hostMutex);
		if (R_SUCCEEDED(gameshareLocalHostGetConnection(&s_host, &connection)))
		{
			std::scoped_lock lock(s_lobbyMutex);
			s_guestFlags = connection.guest_flags;
			if (connection.guest_flags & 1)
			{
				char nickname[33] = {};
				if (R_SUCCEEDED(gameshareLocalHostGetGuestNickname(&s_host, 0, nickname)) && nickname[0])
				{
					std::scoped_lock nameLock(s_nicknameMutex);
					s_guestNickname = nickname;
				}
				const std::string name = SwitchGameShare_GetGuestNickname();
				s_status = name.empty() ? "Guest connected" : "Guest connected: " + name;
			}
			else
				s_status = "Guest disconnected";
		}
	}

	padUpdate(&s_hostPad);
	u64 buttons = padGetButtons(&s_hostPad);
	SwitchGameShareInput guest;
	if (SwitchGameShare_GetGuestInput(guest))
		buttons |= guest.buttons;
	const u64 pressed = buttons & ~s_previousButtons;
	s_previousButtons = buttons;

	std::scoped_lock lock(s_lobbyMutex);
	const int itemCount = (int)s_games.size() + 1;
	if (pressed & HidNpadButton_AnyUp)
		s_selection = (s_selection + itemCount - 1) % itemCount;
	if (pressed & HidNpadButton_AnyDown)
		s_selection = (s_selection + 1) % itemCount;
	if (s_selection == 0 && (pressed & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight | HidNpadButton_A)))
		s_swapRoles.store(!s_swapRoles.load(), std::memory_order_release);
	if (s_selection > 0 && (pressed & HidNpadButton_A))
	{
		choice.launch = true;
		choice.titleId = s_games[s_selection - 1].titleId;
		return true;
	}
	if (pressed & HidNpadButton_B)
	{
		choice.quit = true;
		return true;
	}
	return false;
}

void SwitchGameShare_LobbyRender()
{
	ImFont* font = ImGui_GetFont(28.0f);
	const ImGuiIO& io = ImGui::GetIO();
	ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, IM_COL32(8, 14, 26, 255));
	ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
	ImGui::SetNextWindowSize({std::min(980.0f, io.DisplaySize.x - 80.0f), std::min(640.0f, io.DisplaySize.y - 80.0f)},
	                         ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.0f, 20.0f});
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 20, 34, 245));
	ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(94, 201, 255, 255));
	ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(31, 82, 112, 235));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(31, 82, 112, 235));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(31, 82, 112, 235));
	ImGui::PushFont(font);
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoInputs;
	if (ImGui::Begin("GamePad GameShare lobby", nullptr, flags))
	{
		std::scoped_lock lock(s_lobbyMutex);
		ImGui::TextUnformatted("GamePad GameShare");
		ImGui::TextDisabled("%s", s_status.c_str());
		ImGui::Separator();

		const bool swap = s_swapRoles.load();
		char row[160];
		std::snprintf(row, sizeof(row), "Swap Host and Guest      <  %s  >", swap ? "On" : "Off");
		ImGui::Selectable(row, s_selection == 0, ImGuiSelectableFlags_None, {0.0f, 44.0f});
		if (s_selection == 0)
			ImGui::SetScrollHereY(0.5f);
		ImGui::Separator();
		if (s_games.empty())
			ImGui::TextDisabled(s_scanFinished ? "No games found" : "Scanning for games...");
		for (size_t i = 0; i < s_games.size(); ++i)
		{
			ImGui::Selectable(s_games[i].name.c_str(), s_selection == (int)i + 1, ImGuiSelectableFlags_None,
			                  {0.0f, 44.0f});
			if (s_selection == (int)i + 1)
				ImGui::SetScrollHereY(0.5f);
		}
		ImGui::Separator();
		ImGui::TextDisabled("A  Start / Change      B  End session and return");
	}
	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(2);
}
