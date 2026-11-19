#pragma once

// GamePad GameShare: a Switch 2 hosts a local wireless GameShare session (stpl:u, libounce PR #4)
// and another Nintendo Switch / Switch 2 console acts as the Wii U GamePad.
//
// The core owns the session. The launcher only asks for it (Minus on the game list) and starts the
// core in "lobby" mode; matchmaking, the lobby and the game then all run inside this process, so the
// session survives from lobby to game.

#include <cstdint>
#include <string>

struct SwitchGameShareInput
{
	uint64_t buttons = 0; // HidNpadButton_* bits
	int32_t stick[4] = {}; // left x/y, right x/y (HidAnalogStickState range)
	// Guest touch, in 1280x720 coordinates.
	bool touchDown = false;
	int32_t touchX = 0;
	int32_t touchY = 0;
	// Guest gyro/accelerometer in HOS sensor units (same as HidSixAxisSensorState).
	bool hasMotion = false;
	uint64_t motionSampling = 0;
	float gyro[3] = {};
	float accel[3] = {};
	// Amiibo is not forwarded by GameShare: the NFC reader that works is the host's Joy-Con.
};

enum class SwitchGameShareResult
{
	Ok,
	Cancelled,
	Failed,
};

void SwitchGameShare_Configure(bool lobbyRequested, bool swapRoles);
bool SwitchGameShare_LobbyRequested();
// Off: the guest is the GamePad (host sends the GamePad view). On: host sends the TV view.
bool SwitchGameShare_SwapRoles();
// The guest gets its own picture (GamePad, or TV when swapped) and its own audio, the host keeps the
// other one. If the guest display can't be opened the session falls back to mirroring the host screen.
// True while a separate-screens session is running.
bool SwitchGameShare_SeparateActive();
// True when the renderer should capture this view (padView = GamePad) for the guest.
bool SwitchGameShare_WantsGuestFrame(bool padView);
// One 1280x720 RGBA frame for the guest.
void SwitchGameShare_SubmitGuestFrame(const uint32_t* rgba);
// GamePad audio (48 kHz stereo s16) for the guest. Returns false if it isn't routed there.
bool SwitchGameShare_WriteGuestAudio(const int16_t* frames, uint32_t frameCount);

// Opens the session, shows the system matchmaking applet, and starts streaming once a guest joined.
SwitchGameShareResult SwitchGameShare_Begin(std::string& error);
void SwitchGameShare_End();
// True from a successful Begin until End. While true the TV/GamePad layout is controlled by GameShare.
bool SwitchGameShare_Active();

// Latest guest controller sample. Returns false when no guest sample is available.
bool SwitchGameShare_GetGuestInput(SwitchGameShareInput& out);

// Rumble the guest's controller (0..1).
void SwitchGameShare_SetGuestRumble(float amplitude);
// Guest nickname, empty until connected.
std::string SwitchGameShare_GetGuestNickname();

// ---- Lobby (main thread drives it, the render thread draws it) ----
struct SwitchGameShareLobbyChoice
{
	bool quit = false;
	bool launch = false;
	uint64_t titleId = 0;
};
void SwitchGameShare_LobbyBegin();
// Polls host and guest input and advances the lobby. Returns true once the lobby has a result.
bool SwitchGameShare_LobbyUpdate(SwitchGameShareLobbyChoice& choice);
bool SwitchGameShare_LobbyActive();
void SwitchGameShare_LobbySetActive(bool active);
// Called on the render thread between ImguiBegin/ImguiEnd.
void SwitchGameShare_LobbyRender();
