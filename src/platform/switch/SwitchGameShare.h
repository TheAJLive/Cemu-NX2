#pragma once

// GamePad GameShare: a Switch 2 hosts a local wireless GameShare session (stpl:u, libounce's GameShare host API)
// and another Nintendo Switch / Switch 2 console acts as the Wii U GamePad.
//
// Minus on a game in the launcher starts that game with GameShare. The core creates the renderer, gives the
// guest its own screen (a Vulkan swapchain on the player1 display), runs matchmaking and then boots the game.
// The guest gets the GamePad picture and audio (the TV ones when roles are swapped).

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

constexpr int kSwitchGameShareGuestWidth = 1280;
constexpr int kSwitchGameShareGuestHeight = 720;

void SwitchGameShare_Configure(bool requested, bool swapRoles);
// The launcher asked for this game to run with GameShare.
bool SwitchGameShare_Requested();
// Off: the guest is the GamePad. On: the guest gets the TV picture and the host shows the GamePad.
bool SwitchGameShare_SwapRoles();

// Gives the guest its own screen, shows the system matchmaking applet and starts streaming.
// Call after the renderer exists and before the game starts.
SwitchGameShareResult SwitchGameShare_Begin(std::string& error);
// Closes the session. The guest screen stays until SwitchGameShare_ReleaseGuestScreen.
void SwitchGameShare_End();
// Closes the guest's window. Call after the renderer (and its swapchain on that window) is gone.
void SwitchGameShare_ReleaseGuestScreen();
// True from a successful Begin until End.
bool SwitchGameShare_Active();
// True while the guest has its own screen. If that screen can't be set up the session mirrors the host.
bool SwitchGameShare_SeparateActive();

// GamePad audio (48 kHz stereo s16) for the guest. Returns false if it isn't routed there.
bool SwitchGameShare_WriteGuestAudio(const int16_t* frames, uint32_t frameCount);
// Latest guest controller sample. Returns false when no guest sample is available.
bool SwitchGameShare_GetGuestInput(SwitchGameShareInput& out);
// Rumble the guest's controller (0..1).
void SwitchGameShare_SetGuestRumble(float amplitude);
// Guest nickname, empty until connected.
std::string SwitchGameShare_GetGuestNickname();
// Adds a line to sdmc:/switch/cemu/gameshare-log.txt.
void SwitchGameShare_Log(const char* text);
