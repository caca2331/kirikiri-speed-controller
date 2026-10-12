#include "DirectSoundHook.h"
#include "WasapiHook.h"
#include "SharedSettingsManager.h"
#include "HookUtils.h"
#include "DspRuntime.h"
#include "../common/Logging.h"
#include "../common/SharedSettings.h"
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <Windows.h>
#include <cstring>
#include <winternl.h>
#include <Psapi.h>

// Minimal LDR notification structs to avoid SDK version issues.
typedef struct _KRKR_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} KRKR_UNICODE_STRING, *PKRKR_UNICODE_STRING;

typedef struct _KRKR_LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    const KRKR_UNICODE_STRING *FullDllName;
    const KRKR_UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} KRKR_LDR_DLL_LOADED_NOTIFICATION_DATA, *PKRKR_LDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _KRKR_LDR_DLL_NOTIFICATION_DATA {
    ULONG NotificationReason;
    union {
        KRKR_LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
        KRKR_LDR_DLL_LOADED_NOTIFICATION_DATA Unloaded;
    };
} KRKR_LDR_DLL_NOTIFICATION_DATA, *PKRKR_LDR_DLL_NOTIFICATION_DATA;

namespace {
    FARPROC (WINAPI *g_origGetProcAddress)(HMODULE, LPCSTR) = nullptr;
    HMODULE (WINAPI *g_origLoadLibraryA)(LPCSTR) = nullptr;
    HMODULE (WINAPI *g_origLoadLibraryW)(LPCWSTR) = nullptr;
    HMODULE (WINAPI *g_origLoadLibraryExA)(LPCSTR, HANDLE, DWORD) = nullptr;
    HMODULE (WINAPI *g_origLoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = nullptr;
    PVOID g_dllNotifyCookie = nullptr;
    PVOID g_vectored = nullptr;
    HANDLE g_instanceMutex = nullptr;

    FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR procName);
    HMODULE WINAPI LoadLibraryAHook(LPCSTR lpLibFileName);
    HMODULE WINAPI LoadLibraryWHook(LPCWSTR lpLibFileName);
    HMODULE WINAPI LoadLibraryExAHook(LPCSTR lpLibFileName, HANDLE file, DWORD flags);
    HMODULE WINAPI LoadLibraryExWHook(LPCWSTR lpLibFileName, HANDLE file, DWORD flags);

    // Only faults worth a breadcrumb; benign first-chance exceptions (C++ throws, OutputDebugString,
    // thread naming) would otherwise use up the log slots. Stack overflow is left alone on purpose.
    bool isFatalException(DWORD code) {
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case 0xC0000374: // STATUS_HEAP_CORRUPTION
        case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
            return true;
        default:
            return false;
        }
    }

    LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS *info) {
        static std::atomic<int> logged{0};
        if (!info || !info->ExceptionRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (!isFatalException(code)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (logged.fetch_add(1) < 5) {
            void *addr = info->ExceptionRecord->ExceptionAddress;
            HMODULE mod = nullptr;
            char modName[MAX_PATH] = {};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
                GetModuleBaseNameA(GetCurrentProcess(), mod, modName, static_cast<DWORD>(std::size(modName)));
            }
            KRKR_LOG_ERROR("Vectored exception code=" + krkrspeed::formatHex(code) +
                           " addr=" + krkrspeed::formatHex(reinterpret_cast<std::uintptr_t>(addr)) +
                           " mod=" + modName);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void PatchUnityModuleImports(HMODULE module, const char *nameAnsi) {
        if (!module || !nameAnsi) return;
        krkrspeed::WasapiHook::instance().tryPatchModule(module, nameAnsi);

        bool patched = false;
        patched |= krkrspeed::PatchImportInModule(module, "kernel32.dll", "GetProcAddress",
                                                  reinterpret_cast<void *>(&GetProcAddressHook),
                                                  reinterpret_cast<void **>(&g_origGetProcAddress));
        patched |= krkrspeed::PatchImportInModule(module, "kernel32.dll", "LoadLibraryA",
                                                  reinterpret_cast<void *>(&LoadLibraryAHook),
                                                  reinterpret_cast<void **>(&g_origLoadLibraryA));
        patched |= krkrspeed::PatchImportInModule(module, "kernel32.dll", "LoadLibraryW",
                                                  reinterpret_cast<void *>(&LoadLibraryWHook),
                                                  reinterpret_cast<void **>(&g_origLoadLibraryW));
        patched |= krkrspeed::PatchImportInModule(module, "kernel32.dll", "LoadLibraryExA",
                                                  reinterpret_cast<void *>(&LoadLibraryExAHook),
                                                  reinterpret_cast<void **>(&g_origLoadLibraryExA));
        patched |= krkrspeed::PatchImportInModule(module, "kernel32.dll", "LoadLibraryExW",
                                                  reinterpret_cast<void *>(&LoadLibraryExWHook),
                                                  reinterpret_cast<void **>(&g_origLoadLibraryExW));
        patched |= krkrspeed::PatchImportInModule(module, "kernelbase.dll", "LoadLibraryW",
                                                  reinterpret_cast<void *>(&LoadLibraryWHook),
                                                  reinterpret_cast<void **>(&g_origLoadLibraryW));
        patched |= krkrspeed::PatchImportInModule(module, "kernelbase.dll", "LoadLibraryA",
                                                  reinterpret_cast<void *>(&LoadLibraryAHook),
                                                  reinterpret_cast<void **>(&g_origLoadLibraryA));
        patched |= krkrspeed::PatchImportInModule(module, "kernelbase.dll", "LoadLibraryExW",
                                                  reinterpret_cast<void *>(&LoadLibraryExWHook),
                                                  reinterpret_cast<void **>(&g_origLoadLibraryExW));
        patched |= krkrspeed::PatchImportInModule(module, "kernelbase.dll", "LoadLibraryExA",
                                                  reinterpret_cast<void *>(&LoadLibraryExAHook),
                                                  reinterpret_cast<void **>(&g_origLoadLibraryExA));
        if (patched) {
            KRKR_LOG_INFO(std::string("Patched kernel32/kernelbase imports in ") + nameAnsi);
        }
    }

    void OnLibraryLoaded(HMODULE module, const char *nameAnsi) {
        if (!module || !nameAnsi) return;
        auto &ds = krkrspeed::DirectSoundHook::instance();
        if (!ds.hasCreateHook()) {
            FARPROC fn = GetProcAddress(module, "DirectSoundCreate8");
            if (fn) {
                ds.setOriginalCreate8(reinterpret_cast<void *>(fn));
                KRKR_LOG_INFO(std::string("Captured DirectSoundCreate8 from newly loaded module: ") + nameAnsi);
            }
            FARPROC fn2 = GetProcAddress(module, "DirectSoundCreate");
            if (fn2) {
                ds.setOriginalCreate(reinterpret_cast<void *>(fn2));
                KRKR_LOG_INFO(std::string("Captured DirectSoundCreate from newly loaded module: ") + nameAnsi);
            }
        }

        if (strstr(nameAnsi, "UnityPlayer") || strstr(nameAnsi, "GameAssembly")) {
            PatchUnityModuleImports(module, nameAnsi);
        }
    }

    FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR procName) {
        FARPROC fn = g_origGetProcAddress ? g_origGetProcAddress(module, procName) : nullptr;
        if (!procName || !fn) {
            return fn;
        }

        if (_stricmp(procName, "CoCreateInstance") == 0) {
            krkrspeed::WasapiHook::instance().setOriginalCoCreate(reinterpret_cast<void *>(fn));
            return reinterpret_cast<FARPROC>(&krkrspeed::WasapiHook::CoCreateInstanceHook);
        }
        if (_stricmp(procName, "DirectSoundCreate8") == 0) {
            auto &ds = krkrspeed::DirectSoundHook::instance();
            ds.setOriginalCreate8(reinterpret_cast<void *>(fn));
            return reinterpret_cast<FARPROC>(&krkrspeed::DirectSoundHook::DirectSoundCreate8Hook);
        }
        if (_stricmp(procName, "DirectSoundCreate") == 0) {
            auto &ds = krkrspeed::DirectSoundHook::instance();
            ds.setOriginalCreate(reinterpret_cast<void *>(fn));
            return reinterpret_cast<FARPROC>(&krkrspeed::DirectSoundHook::DirectSoundCreateHook);
        }
        return fn;
    }

    HMODULE WINAPI LoadLibraryAHook(LPCSTR lpLibFileName) {
        HMODULE h = g_origLoadLibraryA ? g_origLoadLibraryA(lpLibFileName) : nullptr;
        if (h && lpLibFileName) {
            OnLibraryLoaded(h, lpLibFileName);
        }
        return h;
    }

    HMODULE WINAPI LoadLibraryWHook(LPCWSTR lpLibFileName) {
        HMODULE h = g_origLoadLibraryW ? g_origLoadLibraryW(lpLibFileName) : nullptr;
        if (h && lpLibFileName) {
            char name[260] = {};
            WideCharToMultiByte(CP_ACP, 0, lpLibFileName, -1, name, static_cast<int>(std::size(name)), nullptr, nullptr);
            OnLibraryLoaded(h, name);
        }
        return h;
    }

    HMODULE WINAPI LoadLibraryExAHook(LPCSTR lpLibFileName, HANDLE file, DWORD flags) {
        HMODULE h = g_origLoadLibraryExA ? g_origLoadLibraryExA(lpLibFileName, file, flags) : nullptr;
        if (h && lpLibFileName) {
            OnLibraryLoaded(h, lpLibFileName);
        }
        return h;
    }

    HMODULE WINAPI LoadLibraryExWHook(LPCWSTR lpLibFileName, HANDLE file, DWORD flags) {
        HMODULE h = g_origLoadLibraryExW ? g_origLoadLibraryExW(lpLibFileName, file, flags) : nullptr;
        if (h && lpLibFileName) {
            char name[260] = {};
            WideCharToMultiByte(CP_ACP, 0, lpLibFileName, -1, name, static_cast<int>(std::size(name)), nullptr, nullptr);
            OnLibraryLoaded(h, name);
        }
        return h;
    }

    // ntdll!LdrRegisterDllNotification types
    typedef VOID(WINAPI *PFN_LdrDllNotification)(ULONG, const KRKR_LDR_DLL_NOTIFICATION_DATA *, PVOID);
    typedef NTSTATUS(WINAPI *PFN_LdrRegisterDllNotification)(ULONG, PFN_LdrDllNotification, PVOID, PVOID *);
    typedef NTSTATUS(WINAPI *PFN_LdrUnregisterDllNotification)(PVOID);

    // A second copy of the hook (e.g. loaded from another path) would chain onto the first and process
    // audio twice, so only the first instance per process installs anything.
    bool claimProcessInstance() {
        const std::wstring name = L"Local\\KrkrSpeedHook_" + std::to_wstring(GetCurrentProcessId());
        g_instanceMutex = CreateMutexW(nullptr, FALSE, name.c_str());
        if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(g_instanceMutex);
            g_instanceMutex = nullptr;
            return false;
        }
        return true;
    }

    // Import-table patches run synchronously inside DllMain: they only rewrite pointers (no loads, no COM),
    // and in --launch mode they are then in place before the suspended game's main thread runs.
    void installImportHooks() {
        if (krkrspeed::PatchImport("kernel32.dll", "GetProcAddress",
                                   reinterpret_cast<void *>(&GetProcAddressHook),
                                   reinterpret_cast<void **>(&g_origGetProcAddress))) {
            KRKR_LOG_INFO("Patched GetProcAddress import to catch dynamic audio API resolution");
        } else {
            KRKR_LOG_WARN("Failed to patch GetProcAddress; dynamic API resolution may bypass hooks");
        }
        if (krkrspeed::PatchImport("kernel32.dll", "LoadLibraryA",
                                   reinterpret_cast<void *>(&LoadLibraryAHook),
                                   reinterpret_cast<void **>(&g_origLoadLibraryA))) {
            KRKR_LOG_INFO("Patched LoadLibraryA to capture late audio module loads");
        } else {
            KRKR_LOG_WARN("Failed to patch LoadLibraryA; late-load modules may be missed");
        }
        if (krkrspeed::PatchImport("kernel32.dll", "LoadLibraryW",
                                   reinterpret_cast<void *>(&LoadLibraryWHook),
                                   reinterpret_cast<void **>(&g_origLoadLibraryW))) {
            KRKR_LOG_INFO("Patched LoadLibraryW to capture late audio module loads");
        } else {
            KRKR_LOG_WARN("Failed to patch LoadLibraryW; late-load modules may be missed");
        }
        krkrspeed::PatchImport("kernelbase.dll", "LoadLibraryW",
                               reinterpret_cast<void *>(&LoadLibraryWHook),
                               reinterpret_cast<void **>(&g_origLoadLibraryW));
        krkrspeed::PatchImport("kernelbase.dll", "LoadLibraryA",
                               reinterpret_cast<void *>(&LoadLibraryAHook),
                               reinterpret_cast<void **>(&g_origLoadLibraryA));
        krkrspeed::PatchImport("kernelbase.dll", "LoadLibraryExW",
                               reinterpret_cast<void *>(&LoadLibraryExWHook),
                               reinterpret_cast<void **>(&g_origLoadLibraryExW));
        krkrspeed::PatchImport("kernelbase.dll", "LoadLibraryExA",
                               reinterpret_cast<void *>(&LoadLibraryExAHook),
                               reinterpret_cast<void **>(&g_origLoadLibraryExA));
        krkrspeed::PatchImport("kernel32.dll", "LoadLibraryExW",
                               reinterpret_cast<void *>(&LoadLibraryExWHook),
                               reinterpret_cast<void **>(&g_origLoadLibraryExW));
        krkrspeed::PatchImport("kernel32.dll", "LoadLibraryExA",
                               reinterpret_cast<void *>(&LoadLibraryExAHook),
                               reinterpret_cast<void **>(&g_origLoadLibraryExA));
        krkrspeed::DirectSoundHook::instance().installImportHooks();
        krkrspeed::WasapiHook::instance().tryPatchModule(GetModuleHandleW(nullptr), "main");
    }

    // Everything that may load modules, create COM/DirectSound objects or block runs off the loader lock.
    void runDeferredInitialization() {
        std::string stage = "start";
        try {
            stage = "vectored handler";
            // Lightweight crash breadcrumbs: log the first few fatal exceptions with addresses.
            g_vectored = AddVectoredExceptionHandler(TRUE, VectoredHandler);

            stage = "module dump";
            try {
                // Dump loaded module names for diagnostics.
                HMODULE mods[256];
                DWORD needed = 0;
                if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
                    size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(mods));
                    std::string msg = "Modules:";
                    char name[MAX_PATH] = {};
                    for (size_t i = 0; i < count; ++i) {
                        if (GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, static_cast<DWORD>(std::size(name))) != 0) {
                            msg += " ";
                            msg += name;
                        }
                    }
                    KRKR_LOG_INFO(msg);
                } else {
                    KRKR_LOG_WARN("EnumProcessModules failed; skipping module dump");
                }
            } catch (...) {
                KRKR_LOG_ERROR("Init: module dump threw an exception; continuing");
            }

            stage = "dsp runtime";
            krkrspeed::EnsureDspRuntime();

            stage = "wasapi init";
            KRKR_LOG_INFO("Init: starting WasapiHook::initialize");
            try {
                krkrspeed::WasapiHook::instance().initialize();
            } catch (...) {
                KRKR_LOG_ERROR("Init: WasapiHook::initialize threw an exception");
            }

            stage = "directsound init";
            KRKR_LOG_INFO("Init: starting DirectSoundHook::initialize");
            try {
                krkrspeed::DirectSoundHook::instance().initialize();
            } catch (...) {
                KRKR_LOG_ERROR("Init: DirectSoundHook::initialize threw an exception");
            }

            stage = "ldr notify";
            KRKR_LOG_INFO("Init: registering LdrRegisterDllNotification");
            try {
                HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                auto reg = reinterpret_cast<PFN_LdrRegisterDllNotification>(
                    GetProcAddress(ntdll, "LdrRegisterDllNotification"));
                if (reg) {
                    NTSTATUS st = reg(0, [](ULONG reason, const KRKR_LDR_DLL_NOTIFICATION_DATA *data, PVOID) {
                        if (reason != 1 || !data || !data->Loaded.FullDllName || !data->Loaded.FullDllName->Buffer) return;
                        const auto *full = data->Loaded.FullDllName;
                        std::wstring wname(full->Buffer, full->Length / sizeof(wchar_t));
                        if (!wname.empty()) {
                            char name[260] = {};
                            WideCharToMultiByte(CP_ACP, 0, wname.c_str(), -1, name, static_cast<int>(std::size(name)), nullptr, nullptr);
                            OnLibraryLoaded(reinterpret_cast<HMODULE>(data->Loaded.DllBase), name);
                        }
                    }, nullptr, &g_dllNotifyCookie);
                    if (st != 0) {
                        KRKR_LOG_WARN("LdrRegisterDllNotification returned status " + krkrspeed::formatHex(static_cast<ULONG>(st)));
                    } else {
                        KRKR_LOG_INFO("Init: LdrRegisterDllNotification registered");
                    }
                } else {
                    KRKR_LOG_WARN("Init: LdrRegisterDllNotification not found");
                }
            } catch (...) {
                KRKR_LOG_ERROR("Init: LdrRegisterDllNotification threw an exception");
            }

            stage = "done";
            KRKR_LOG_INFO("Hook initialization thread finished");
        } catch (...) {
            KRKR_LOG_ERROR("Hook initialization thread crashed at stage: " + stage);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        wchar_t skipFlag[8] = {};
        if (GetEnvironmentVariableW(L"KRKR_SKIP_HOOK_INIT", skipFlag, static_cast<DWORD>(std::size(skipFlag))) > 0) {
            KRKR_LOG_INFO("KRKR_SKIP_HOOK_INIT set; skipping hook setup in this process");
            return TRUE;
        }

        DisableThreadLibraryCalls(hModule);
        if (!claimProcessInstance()) {
            // Stay loaded but inert; the first instance already owns every hook.
            return TRUE;
        }
        try {
            // Reads the controller's settings (and enables logging when requested) before anything logs.
            krkrspeed::SharedSettingsManager::instance().attachSharedSettings();
            KRKR_LOG_INFO("krkr_speed_hook.dll attached; installing import hooks");
            installImportHooks();
        } catch (...) {
            KRKR_LOG_ERROR("Import hook installation threw an exception");
        }
        std::thread(runDeferredInitialization).detach();
    }
    return TRUE;
}
