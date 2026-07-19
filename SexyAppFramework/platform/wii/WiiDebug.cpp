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

static void DrawState()
{
	SexyAppBase* anApp = gSexyAppBase;
	if (anApp == NULL || anApp->mGLInterface == NULL)
		return;

	GLInterface* aGL = anApp->mGLInterface;

	// Top row: AddPakFile("main.pak") progress 0-9 (see PakInterface.cpp for
	// what each step means; full green bar = pak loaded successfully)
	aGL->FillRect(Rect(10, 10, 9 * 20, 20), Color(64, 64, 64), Graphics::DRAWMODE_NORMAL);
	aGL->FillRect(Rect(10, 10, ::gWiiDebugPakStep * 20, 20), Color(0, 255, 0), Graphics::DRAWMODE_NORMAL);

	// Second row: one square per boot checkpoint reached (cyan) vs not (dark)
	for (int i = 0; i < 20; i++)
	{
		bool aHit = (gCheckpointMask >> i) & 1;
		aGL->FillRect(Rect(10 + i * 22, 40, 18, 18), aHit ? Color(0, 200, 255) : Color(48, 48, 48), Graphics::DRAWMODE_NORMAL);
	}

	// Third row: how many files main.pak's TOC actually registered (orange
	// bar, 1px per file up to 300) - if this is ~0 despite row 1 filling all
	// the way, the TOC loop "succeeded" without ever creating usable records
	int aRecordCount = (int)gPakInterface->mPakRecordMap.size();
	int aBarWidth = aRecordCount > 300 ? 300 : aRecordCount;
	aGL->FillRect(Rect(10, 70, 300, 20), Color(64, 64, 64), Graphics::DRAWMODE_NORMAL);
	aGL->FillRect(Rect(10, 70, aBarWidth, 20), Color(255, 200, 0), Graphics::DRAWMODE_NORMAL);

	// Fourth row: does gPakInterface->FOpen("properties/resources.xml") find
	// anything at all, straight from the pak record map / loose-file fallback
	// - green = found something, red = not found by either path
	PFILE* aTestFile = gPakInterface->FOpen("properties/resources.xml", "rb");
	aGL->FillRect(Rect(10, 100, 40, 20), aTestFile ? Color(0, 255, 0) : Color(255, 0, 0), Graphics::DRAWMODE_NORMAL);

	// Fifth+sixth rows: the first two raw bytes actually read out of that
	// file, one row of 8 bit-squares each (MSB first) - if this is a real
	// resources.xml the first byte should be '<' (0x3C = 00111100) or an
	// XML/UTF BOM byte (0xEF/0xFF/0xFE); anything else means we're reading
	// garbage (wrong offset/size) rather than genuinely missing content
	if (aTestFile)
	{
		unsigned char aBytes[2] = {0, 0};
		gPakInterface->FRead(aBytes, 1, 2, aTestFile);
		for (int aByteIdx = 0; aByteIdx < 2; aByteIdx++)
		{
			for (int aBit = 0; aBit < 8; aBit++)
			{
				bool aSet = (aBytes[aByteIdx] >> (7 - aBit)) & 1;
				aGL->FillRect(Rect(10 + aBit * 22, 130 + aByteIdx * 30, 18, 18), aSet ? Color(255, 0, 128) : Color(48, 48, 48), Graphics::DRAWMODE_NORMAL);
			}
		}
		gPakInterface->FClose(aTestFile);
	}

	aGL->Redraw();
}

void WiiDebugCheckpoint(int theId)
{
	gCheckpointMask |= 1u << theId;
	DrawState();
}

void WiiDebugHalt()
{
	DrawState();
	for (;;)
		VIDEO_WaitVSync();
}
