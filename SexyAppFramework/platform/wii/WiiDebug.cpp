#include "WiiDebug.h"

#include <gccore.h>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/Graphics.h"
#include "graphics/Color.h"
#include "misc/Rect.h"
#include "paklib/PakInterface.h"

using namespace Sexy;

static unsigned int gCheckpointMask = 0;

// Logical screen is only 400px wide (800/2 from DOWNSCALE_COUNT) - anything
// drawn past x=400 is silently off-screen. Keep every row within that.
#define WII_DEBUG_SCREEN_WIDTH 400

static void DrawState()
{
	SexyAppBase* anApp = gSexyAppBase;
	if (anApp == NULL || anApp->mGLInterface == NULL)
		return;

	GLInterface* aGL = anApp->mGLInterface;

	// Row 1 (y=10): AddPakFile("main.pak") progress 0-9 (see PakInterface.cpp
	// for what each step means; full green bar = pak loaded successfully)
	aGL->FillRect(Rect(10, 10, 9 * 20, 20), Color(64, 64, 64), Graphics::DRAWMODE_NORMAL);
	aGL->FillRect(Rect(10, 10, ::gWiiDebugPakStep * 20, 20), Color(0, 255, 0), Graphics::DRAWMODE_NORMAL);

	// Rows 2-3 (y=35,58): one square per boot checkpoint reached (cyan) vs
	// not (dark), wrapped at 17/row so nothing goes past x=400.
	// 1-5 = boot sequence, 6/13 = ParseResourcesFile true/false, 7 =
	// ShowResourceError returned, 8/14 = TodLoadResources true/false,
	// 9-12 = Start/DoMainLoop, 15-18 = ProcessDeferredMessages substeps,
	// 20/21 = about-to/returned DoLoadImage, 22/23 = DoLoadSound, 24/25 = DoLoadFont
	{
		const int perRow = 17;
		for (int i = 0; i < 34; i++)
		{
			bool aHit = (gCheckpointMask >> i) & 1;
			int aRow = i / perRow;
			int aCol = i % perRow;
			aGL->FillRect(Rect(10 + aCol * 22, 35 + aRow * 23, 18, 18), aHit ? Color(0, 200, 255) : Color(48, 48, 48), Graphics::DRAWMODE_NORMAL);
		}
	}

	// Row 4 (y=81): how many files main.pak's TOC actually registered (orange
	// bar, 1px per file up to 380) - if this is ~0 despite row 1 filling all
	// the way, the TOC loop "succeeded" without ever creating usable records
	int aRecordCount = (int)gPakInterface->mPakRecordMap.size();
	int aBarWidth = aRecordCount > 380 ? 380 : aRecordCount;
	aGL->FillRect(Rect(10, 81, 380, 20), Color(64, 64, 64), Graphics::DRAWMODE_NORMAL);
	aGL->FillRect(Rect(10, 81, aBarWidth, 20), Color(255, 200, 0), Graphics::DRAWMODE_NORMAL);

	// Row 5 (y=106): does gPakInterface->FOpen("properties/resources.xml")
	// find anything at all - green = found something, red = not found
	PFILE* aTestFile = gPakInterface->FOpen("properties/resources.xml", "rb");
	aGL->FillRect(Rect(10, 106, 40, 20), aTestFile ? Color(0, 255, 0) : Color(255, 0, 0), Graphics::DRAWMODE_NORMAL);

	// Rows 6-7 (y=131,154): the first two raw bytes actually read out of that
	// file, one row of 8 bit-squares each (MSB first) - '<' '?' (00111100
	// 00111111) confirms real XML content is being read correctly
	if (aTestFile)
	{
		unsigned char aBytes[2] = {0, 0};
		gPakInterface->FRead(aBytes, 1, 2, aTestFile);
		for (int aByteIdx = 0; aByteIdx < 2; aByteIdx++)
		{
			for (int aBit = 0; aBit < 8; aBit++)
			{
				bool aSet = (aBytes[aByteIdx] >> (7 - aBit)) & 1;
				aGL->FillRect(Rect(10 + aBit * 22, 131 + aByteIdx * 23, 18, 18), aSet ? Color(255, 0, 128) : Color(48, 48, 48), Graphics::DRAWMODE_NORMAL);
			}
		}
		gPakInterface->FClose(aTestFile);
	}

	// Row 8 (y=177): TodLoadNextResource() call count (1px per call, capped
	// at 380) - if this climbs steadily the resource load is just slow; if
	// it freezes at a fixed number, that call is genuinely stuck
	int aLoopCount = ::gWiiDebugResourceLoopCount;
	int aLoopBarWidth = aLoopCount > 380 ? 380 : aLoopCount;
	aGL->FillRect(Rect(10, 177, 380, 20), Color(64, 64, 64), Graphics::DRAWMODE_NORMAL);
	aGL->FillRect(Rect(10, 177, aLoopBarWidth, 20), Color(128, 255, 128), Graphics::DRAWMODE_NORMAL);

	// WII DEBUG VERIFY MARKER (y=202) - big white square in a spot nothing
	// else uses. Only exists in this exact build; if you don't see this,
	// you're looking at an old cached .dol, not the one just built.
	aGL->FillRect(Rect(340, 202, 50, 30), Color(255, 255, 255), Graphics::DRAWMODE_NORMAL);

	aGL->Redraw();
}

void WiiDebugCheckpoint(int theId)
{
	gCheckpointMask |= 1u << theId;
	DrawState();
}

void WiiDebugRedraw()
{
	DrawState();
}

void WiiDebugHalt()
{
	DrawState();
	for (;;)
		VIDEO_WaitVSync();
}
