#pragma once
#include <cstdint>


void SwitchThread_RestoreProcessAffinity();

int SwitchThread_AllowedCoreCount();

// Cores the process may use besides the three application cores.
int SwitchThread_HelperCoreCount();

// Size for a pool of background workers: one per helper core, clamped.
uint32_t SwitchThread_WorkerCount(uint32_t minimum, uint32_t maximum);

// Restrict the calling thread to the three application cores.
void SwitchThread_PinToAppCores(const char* what);

bool SwitchThread_PinToHelperCore(const char* what, int32_t priority);

void SwitchThread_AllowHelperCore(const char* what);

#if defined(__SWITCH2__)
void SwitchThread_PinGpuThread();

// Move the calling thread onto the helper cores at its current priority.
void SwitchThread_UseHelperCores(const char* what);
#endif
