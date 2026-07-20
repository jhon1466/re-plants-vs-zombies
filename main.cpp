#include "LawnApp.h"
#include "Resources.h"
#include "Sexy.TodLib/TodStringFile.h"
using namespace Sexy;

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
extern "C" {
	unsigned int __stacksize__ = 512 * 1024;
}
#endif

#ifdef NINTENDO_WII
#include <fat.h>
#endif

#ifdef __SWITCH__
#include <switch.h>
// The default libnx heap init dynamically grabs whatever memory is
// available, but real hardware appears to hand this homebrew a much
// tighter pool than what we saw under Ryujinx (heap was already nearly
// exhausted well before this point in testing). Explicitly request a
// large fixed heap up front instead of relying on auto-sizing.
extern "C"
{
	u32 __nx_applet_type = AppletType_Application;

	#define INNER_HEAP_SIZE 0x10000000 // 256 MiB
	size_t nx_inner_heap_size = INNER_HEAP_SIZE;
	char nx_inner_heap[INNER_HEAP_SIZE];

	void __libnx_initheap(void)
	{
		void* addr = nx_inner_heap;
		size_t size = nx_inner_heap_size;

		extern char* fake_heap_start;
		extern char* fake_heap_end;

		fake_heap_start = (char*)addr;
		fake_heap_end = (char*)addr + size;
	}
}
#endif

bool (*gAppCloseRequest)();				//[0x69E6A0]
bool (*gAppHasUsedCheatKeys)();			//[0x69E6A4]
SexyString (*gGetCurrentLevelName)();

//0x44E8F0
//int WINAPI WinMain(_In_ HINSTANCE /* hInstance */, _In_opt_ HINSTANCE /* hPrevInstance */, _In_ LPSTR /* lpCmdLine */, _In_ int /* nCmdShow */)
int main(int argc, char** argv)
{
#ifdef __3DS__
	osSetSpeedupEnable(true);
#endif

#ifdef NINTENDO_WII
	// mounts sdmc:/ - must happen before anything (chdir, AddPakFile) touches
	// the SD card; SexyAppBase::Init() does both well before MakeWindow() runs
	fatInitDefault();
#endif

	TodStringListSetColors(gLawnStringFormats, gLawnStringFormatCount);
	gGetCurrentLevelName = LawnGetCurrentLevelName;
	gAppCloseRequest = LawnGetCloseRequest;
	gAppHasUsedCheatKeys = LawnHasUsedCheatKeys;
	gExtractResourcesByName = Sexy::ExtractResourcesByName;
	gLawnApp = new LawnApp();
	gLawnApp->Init();
	gLawnApp->Start();
	gLawnApp->Shutdown();
	if (gLawnApp)
		delete gLawnApp;

	return 0;
};
