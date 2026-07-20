#ifndef __SEXY_WII_MEM2ALLOC_H__
#define __SEXY_WII_MEM2ALLOC_H__

#include <cstddef>

// MEM2 (the 64MB auxiliary RAM pool) is not part of the standard newlib
// malloc() heap on Wii - that only covers MEM1 (24MB), which is nowhere
// near enough to hold something like main.pak (tens of MB) in one piece.
// These allocate straight out of MEM2 via libogc's lightweight heap
// allocator, for anything too big to reasonably live in MEM1.
void* Mem2Alloc(size_t theSize);
void  Mem2Free(void* thePtr);

#endif // __SEXY_WII_MEM2ALLOC_H__
