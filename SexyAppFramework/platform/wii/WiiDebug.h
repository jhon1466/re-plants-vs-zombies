#ifndef __SEXY_WII_DEBUG_H__
#define __SEXY_WII_DEBUG_H__

// Temporary boot diagnostics for the Wii port. Every checkpoint accumulates
// into a bitmask and the FULL state (pak-load progress bar + one square per
// checkpoint reached) is redrawn each time, so the screen always shows the
// complete history rather than just the last marker. Safe to call before the
// renderer exists - state is recorded and shows up on the first drawable call.
void WiiDebugCheckpoint(int theId);

// Redraws the current state without recording a new checkpoint - call
// periodically from inside a long-running loop to see live progress
// (e.g. gWiiDebugResourceLoopCount) instead of a static last-known frame.
void WiiDebugRedraw();

// Draw the final state and spin forever (instead of exit()) so the screen
// keeps showing how far boot got.
void WiiDebugHalt();

// Bumped once per TodResourceManager::TodLoadNextResource() call (global
// namespace - TodCommon.cpp has no "using namespace Sexy;") so we can tell
// a slow-but-progressing resource load apart from a stuck one.
extern int gWiiDebugResourceLoopCount;

// Bumped once per outer for(;;) iteration in GetGIFImage's block-skipping
// loop (ImageLib.cpp, global namespace - that file doesn't have "using
// namespace Sexy;" either). Distinguishes a genuinely stuck GIF parse from
// one that's just iterating a very long extension-block chain.
extern int gWiiDebugGifLoopCount;

#endif // __SEXY_WII_DEBUG_H__
