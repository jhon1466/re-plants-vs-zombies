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
	for (int i = 0; i < 16; i++)
	{
		bool aHit = (gCheckpointMask >> i) & 1;
		aGL->FillRect(Rect(10 + i * 22, 40, 18, 18), aHit ? Color(0, 200, 255) : Color(48, 48, 48), Graphics::DRAWMODE_NORMAL);
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
