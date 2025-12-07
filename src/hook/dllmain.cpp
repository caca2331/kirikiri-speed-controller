#include "XAudio2Hook.h"
#include "DirectSoundHook.h"
#include <thread>
#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        std::thread([] {
            krkrspeed::XAudio2Hook::instance().initialize();
            krkrspeed::DirectSoundHook::instance().initialize();
        }).detach();
    }
    return TRUE;
}
