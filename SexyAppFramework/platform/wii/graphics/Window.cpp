#include <gccore.h>
#include <fat.h>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "graphics/Graphics.h"
#include "graphics/Color.h"
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

		// WII DEBUG: magenta breadcrumb square = VIDEO/GX init + renderer confirmed alive
		mGLInterface->FillRect(Rect(10, 10, 40, 40), Color(255, 0, 255), Graphics::DRAWMODE_NORMAL);
		mGLInterface->Redraw();

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
