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

bool isDelayLoadFailure(DWORD code) {
    return code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
           code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND);
}

// The delay-load helper reports a missing DLL or export by raising an SEH exception, not by returning an
// error, and C++ catch blocks do not see it. Catch exactly those two so a missing or incompatible
// SoundTouch.dll (e.g. a game's own copy) disables DSP instead of crashing the game.
HRESULT loadSoundTouchImports(bool &raised) {
    raised = false;
#ifdef _MSC_VER
    __try {
        return __HrLoadAllImportsForDll(kSoundTouchDll);
    } __except (isDelayLoadFailure(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        raised = true;
        return HRESULT_FROM_WIN32(HRESULT_CODE(GetExceptionCode()));
    }
#else
    return __HrLoadAllImportsForDll(kSoundTouchDll);
#endif
}

} // namespace

bool EnsureDspRuntime() {
    static std::once_flag once;
    static bool available = false;
    std::call_once(once, [] {
        bool raised = false;
        const HRESULT hr = loadSoundTouchImports(raised);
        // Builds without /DELAYLOAD import SoundTouch statically; it is then already loaded. After a raised
        // failure the module may be loaded yet lack exports, so it must not count as available.
        available = SUCCEEDED(hr) || (!raised && GetModuleHandleA(kSoundTouchDll) != nullptr);
        if (available) {
            KRKR_LOG_INFO("SoundTouch runtime loaded");
        } else {
            KRKR_LOG_ERROR("SoundTouch.dll could not be loaded (hr=" + formatHex(static_cast<std::uint32_t>(hr)) +
                           (raised ? ", missing DLL or export" : "") +
                           "); audio processing disabled. Keep SoundTouch.dll next to krkr_speed_hook.dll");
        }
    });
    return available;
}

} // namespace krkrspeed

// Picked up by the delay-load helper (delayimp.lib) for every delay-loaded import of this module.
ExternC const PfnDliHook __pfnDliNotifyHook2 = krkrspeed::DelayLoadNotify;
