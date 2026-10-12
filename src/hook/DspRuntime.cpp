#include "DspRuntime.h"
#include "../common/Logging.h"

#include <Windows.h>
#include <delayimp.h>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace krkrspeed {
namespace {

constexpr char kSoundTouchDll[] = "SoundTouch.dll";

std::filesystem::path hookDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&hookDirectory), &self)) {
        return {};
    }
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(self, buffer, MAX_PATH) == 0) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
}

FARPROC WINAPI DelayLoadNotify(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (dliNotify != dliNotePreLoadLibrary || !pdli || !pdli->szDll || _stricmp(pdli->szDll, kSoundTouchDll) != 0) {
        return nullptr;
    }
    const auto dir = hookDirectory();
    if (dir.empty()) {
        return nullptr;
    }
    const auto path = dir / kSoundTouchDll;
    // Returning nullptr falls back to the default search order (game folder, system, PATH).
    return reinterpret_cast<FARPROC>(LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

} // namespace

bool EnsureDspRuntime() {
    static std::once_flag once;
    static bool available = false;
    std::call_once(once, [] {
        const HRESULT hr = __HrLoadAllImportsForDll(kSoundTouchDll);
        // Builds without /DELAYLOAD import SoundTouch statically; it is then already loaded.
        available = SUCCEEDED(hr) || GetModuleHandleA(kSoundTouchDll) != nullptr;
        if (available) {
            KRKR_LOG_INFO("SoundTouch runtime loaded");
        } else {
            KRKR_LOG_ERROR("SoundTouch.dll could not be loaded (hr=" + formatHex(static_cast<std::uint32_t>(hr)) +
                           "); audio processing disabled. Keep SoundTouch.dll next to krkr_speed_hook.dll");
        }
    });
    return available;
}

} // namespace krkrspeed

// Picked up by the delay-load helper (delayimp.lib) for every delay-loaded import of this module.
ExternC const PfnDliHook __pfnDliNotifyHook2 = krkrspeed::DelayLoadNotify;
