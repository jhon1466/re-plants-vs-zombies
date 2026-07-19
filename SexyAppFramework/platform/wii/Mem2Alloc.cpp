#include "Mem2Alloc.h"

#include <cstdint>
#include <gccore.h>
#include <ogc/lwp_heap.h>

static heap_cntrl gMem2Heap;
static bool gMem2HeapInited = false;

static void Mem2HeapInit()
{
	if (gMem2HeapInited)
		return;
	gMem2HeapInited = true;

	void* aLo = SYS_GetArena2Lo();
	void* aHi = SYS_GetArena2Hi();
	__lwp_heap_init(&gMem2Heap, aLo, (u32)((uintptr_t)aHi - (uintptr_t)aLo), 32);
}

void* Mem2Alloc(size_t theSize)
{
	Mem2HeapInit();
	return __lwp_heap_allocate(&gMem2Heap, theSize);
}

void Mem2Free(void* thePtr)
{
	if (thePtr)
		__lwp_heap_free(&gMem2Heap, thePtr);
}
