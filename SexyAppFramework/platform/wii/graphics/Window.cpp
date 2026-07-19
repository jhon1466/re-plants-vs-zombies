#include <gccore.h>
#include <fat.h>
#include <cstdio>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

void SexyAppBase::MakeWindow()
{
	if (mGLInterface == NULL)
	{
		// WII DEBUG: redirect stdout/stderr to Dolphin's OSReport log so printf is visible there
		SYS_STDIO_Report(true);
		setvbuf(stdout, NULL, _IONBF, 0);
		printf("WII DEBUG: MakeWindow() start\n");

		// mounts the SD card (sdmc:/) so main.pak/properties can be read; must
		// happen before anything under Resources.cpp tries to open a file
		bool aFatOk = fatInitDefault();
		printf("WII DEBUG: fatInitDefault() = %d\n", (int)aFatOk);

		mGLInterface = new GLInterface(this);
		printf("WII DEBUG: GLInterface constructed\n");
		InitGLInterface();
		printf("WII DEBUG: InitGLInterface() done\n");

		mGLInterface->UpdateViewport();
		mWidgetManager->Resize(mScreenBounds, mGLInterface->mPresentationRect);
	}

	bool isActive = mActive;
	mActive = true;

	mPhysMinimized = false;

	if (isActive != mActive)
		RehupFocus();

	ReInitImages();

	mWidgetManager->mImage = mGLInterface->GetScreenImage();
	mWidgetManager->MarkAllDirty();
}
