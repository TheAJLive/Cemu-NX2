#include "platform/switch/SwitchGameShare.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
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

std::atomic_bool s_lobbyRequested{false};
std::atomic_bool s_swapRoles{false};
std::atomic_bool s_active{false};
std::atomic_bool s_lobbyActive{false};
// pad the guest is using, for rumble
std::atomic<int> s_guestPadId{(int)HidNpadIdType_No1};
std::atomic<u32> s_guestPadStyle{0};
std::mutex s_nicknameMutex;
std::string s_guestNickname;

GameShareLocalHost s_host{};
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
} // namespace

void SwitchGameShare_Configure(bool lobbyRequested, bool swapRoles)
{
	s_lobbyRequested.store(lobbyRequested, std::memory_order_release);
	s_swapRoles.store(swapRoles, std::memory_order_release);
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
	rc = gameshareLocalHostPrepare(&s_host, 1, &audio);
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

	rc = gameshareLocalHostStart(&s_host);
	if (R_FAILED(rc))
	{
		error = DescribeFailure("starting the stream", rc);
		WriteErrorLog(error);
		SwitchGameShare_End();
		return SwitchGameShareResult::Failed;
	}
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
	const Result rc = gameshareLocalHostClose(&s_host);
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
