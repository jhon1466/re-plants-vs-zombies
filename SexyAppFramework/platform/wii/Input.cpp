#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <unordered_map>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "graphics/Graphics.h"
#include "graphics/Color.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

// Wiimote buttons that don't map to the pointer are funneled onto the same
// "keyboard" key-code space the widget system already understands, mirroring
// how the Switch backend remaps HidNpadButton_* onto KeyCode.
static std::unordered_map<u32, KeyCode> wpadKeyMaps = {
	{WPAD_BUTTON_HOME,  KEYCODE_ESCAPE},
	{WPAD_BUTTON_MINUS, KEYCODE_SPACE},
	{WPAD_BUTTON_PLUS,  KEYCODE_RETURN},
};

// GameCube controller fallback (Wii also accepts GC pads in the side ports)
static std::unordered_map<u16, KeyCode> padKeyMaps = {
	{PAD_BUTTON_START, KEYCODE_ESCAPE},
	{PAD_BUTTON_A,     KEYCODE_RETURN},
	{PAD_TRIGGER_L,    KEYCODE_LBUTTON},
	{PAD_TRIGGER_R,    KEYCODE_RBUTTON},
};

void SexyAppBase::InitInput()
{
	WPAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);

	// IR pointer coordinates are reported in this "virtual resolution" space;
	// mGLInterface is already created by MakeWindow() by the time InitInput() runs
	int screenWidth = mGLInterface ? mGLInterface->mDisplayWidth : 640;
	int screenHeight = mGLInterface ? mGLInterface->mDisplayHeight : 480;
	WPAD_SetVRes(WPAD_CHAN_ALL, screenWidth, screenHeight);

	PAD_Init();

	if (!mMouseIn)
		mMouseIn = true;
}

bool SexyAppBase::StartTextInput(std::string& theInput)
{
	// no on-screen keyboard available in vanilla libogc/wiiuse
	return false;
}

void SexyAppBase::StopTextInput()
{

}

bool SexyAppBase::ProcessDeferredMessages(bool singleMessage)
{
#ifdef NINTENDO_WII
	// WII DEBUG: row 5, yellow, sub-steps - only drawn on the very first call
	// (this runs every frame), to see if WPAD/PAD polling is what hangs
	static bool sWiiDebugFirstCall = true;
	bool aIsFirstCall = sWiiDebugFirstCall;
	if (aIsFirstCall)
	{
		sWiiDebugFirstCall = false;
		mGLInterface->FillRect(Rect(10, 140, 1 * 20, 20), Color(255, 255, 0), Graphics::DRAWMODE_NORMAL);
		mGLInterface->Redraw();
	}
#endif

	WPAD_ScanPads();
	PAD_ScanPads();

#ifdef NINTENDO_WII
	if (aIsFirstCall)
	{
		mGLInterface->FillRect(Rect(10, 140, 2 * 20, 20), Color(255, 255, 0), Graphics::DRAWMODE_NORMAL);
		mGLInterface->Redraw();
	}
#endif

	u32 wpadDown = WPAD_ButtonsDown(WPAD_CHAN_0);
	u32 wpadUp = WPAD_ButtonsUp(WPAD_CHAN_0);
	u16 padDown = PAD_ButtonsDown(PAD_CHAN0);
	u16 padUp = PAD_ButtonsUp(PAD_CHAN0);

	if (wpadDown || padDown)
	{
		mLastUserInputTick = mLastTimerTime;

		for (auto& k : wpadKeyMaps)
			if (wpadDown & k.first)
				mWidgetManager->KeyDown(k.second);

		for (auto& k : padKeyMaps)
			if (padDown & k.first)
				mWidgetManager->KeyDown(k.second);
	}

	if (wpadUp || padUp)
	{
		mLastUserInputTick = mLastTimerTime;

		for (auto& k : wpadKeyMaps)
			if (wpadUp & k.first)
				mWidgetManager->KeyUp(k.second);

		for (auto& k : padKeyMaps)
			if (padUp & k.first)
				mWidgetManager->KeyUp(k.second);
	}

	// Wiimote IR acts as the mouse pointer; A/B (the two "trigger" buttons
	// under the pointing hand) act as the left click while it's on-screen
	static bool prevPointerDown = false;
	static int x = 0, y = 0;

	ir_t ir;
	WPAD_IR(WPAD_CHAN_0, &ir);

#ifdef NINTENDO_WII
	if (aIsFirstCall)
	{
		mGLInterface->FillRect(Rect(10, 140, 3 * 20, 20), Color(255, 255, 0), Graphics::DRAWMODE_NORMAL);
		mGLInterface->Redraw();
	}
#endif

	if (ir.valid)
	{
		mLastUserInputTick = mLastTimerTime;

		x = (int)ir.x;
		y = (int)ir.y;
		mWidgetManager->RemapMouse(x, y);
		mWidgetManager->MouseMove(x, y);
	}

	u32 wpadHeld = WPAD_ButtonsHeld(WPAD_CHAN_0);
	bool pointerDown = ir.valid && (wpadHeld & (WPAD_BUTTON_A | WPAD_BUTTON_B));

	if (pointerDown && !prevPointerDown)
		mWidgetManager->MouseDown(x, y, 1);
	else if (!pointerDown && prevPointerDown)
		mWidgetManager->MouseUp(x, y, 1);

	prevPointerDown = pointerDown;

#ifdef NINTENDO_WII
	if (aIsFirstCall)
	{
		mGLInterface->FillRect(Rect(10, 140, 4 * 20, 20), Color(255, 255, 0), Graphics::DRAWMODE_NORMAL);
		mGLInterface->Redraw();
	}
#endif

	return false;
}
