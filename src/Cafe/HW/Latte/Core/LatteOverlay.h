#pragma once

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);
void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);
// Frames per second that showed a new image (the FPS line counts every swap).
void LatteOverlay_setRenderedFps(double fps);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);