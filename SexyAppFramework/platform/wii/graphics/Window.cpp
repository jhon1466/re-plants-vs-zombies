#include <gccore.h>
#include <fat.h>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

void SexyAppBase::MakeWindow()
{
	if (mGLInterface == NULL)
	{
		// mounts the SD card (sdmc:/) so main.pak/properties can be read; must
		// happen before anything under Resources.cpp tries to open a file
		fatInitDefault();

		mGLInterface = new GLInterface(this);
		InitGLInterface();

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
