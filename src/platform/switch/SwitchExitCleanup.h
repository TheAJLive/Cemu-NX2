#pragma once

// Switch 2: hbloader runs the next program (hbmenu or the launcher) in this
// process, on the heap we leave behind. A pthread that was never joined keeps
// its stack mirrored into the stack region, and a mirrored stack keeps its
// heap pages locked, so the next program comes up short of memory. This
// pauses the threads that are still running and unmaps every leftover
// stack mirror. Call it last, right before the process returns to hbloader.
void SwitchExitCleanup_Run();
