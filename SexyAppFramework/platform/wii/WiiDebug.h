#ifndef __SEXY_WII_DEBUG_H__
#define __SEXY_WII_DEBUG_H__

// Temporary boot diagnostics for the Wii port. Every checkpoint accumulates
// into a bitmask and the FULL state (pak-load progress bar + one square per
// checkpoint reached) is redrawn each time, so the screen always shows the
// complete history rather than just the last marker. Safe to call before the
// renderer exists - state is recorded and shows up on the first drawable call.
void WiiDebugCheckpoint(int theId);

// Draw the final state and spin forever (instead of exit()) so the screen
// keeps showing how far boot got.
void WiiDebugHalt();

#endif // __SEXY_WII_DEBUG_H__
