#include "ControllerCore.h"
#include "ControllerInternal.h"
#include "../common/SharedSettings.h"
#include "../common/SharedStatus.h"
#include "../common/Logging.h"
#include "KrkrVersion.h"
#include <TlHelp32.h>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cmath>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <sstream>
#include <iomanip>
#include <shellapi.h>

namespace krkrspeed::controller {
namespace {

using detail::isProcessBlacklisted;

// The injector itself waits up to 10 s for the remote LoadLibraryW; give it time to report back.
constexpr DWORD kInjectorWaitMs = 15000;

ProcessArch classifyMachine(USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386: return ProcessArch::X86;
    case IMAGE_FILE_MACHINE_AMD64: return ProcessArch::X64;
    case IMAGE_FILE_MACHINE_ARM64: return ProcessArch::Arm64;
    default: return ProcessArch::Unknown;
    }
}

// One EnumWindows pass: title of the first visible top-level window (in z-order) for each process.
std::unordered_map<DWORD, std::wstring> collectVisibleWindowTitles() {
    std::unordered_map<DWORD, std::wstring> titles;
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto *map = reinterpret_cast<std::unordered_map<DWORD, std::wstring> *>(lParam);
            if (!IsWindowVisible(hwnd)) {
                return TRUE;
            }
            DWORD windowPid = 0;
            GetWindowThreadProcessId(hwnd, &windowPid);
            if (windowPid == 0 || map->count(windowPid) != 0) {
                return TRUE;
            }
            wchar_t buf[256] = {};
            GetWindowTextW(hwnd, buf, static_cast<int>(std::size(buf)));
            map->emplace(windowPid, buf);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&titles));
    return titles;
}

std::filesystem::path getProcessDirectory(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (QueryFullProcessImageNameW(process, 0, buffer, &size) == 0) {
        CloseHandle(process);
        return {};
    }
    CloseHandle(process);
    std::filesystem::path exePath(buffer);
    return exePath.parent_path();
}

std::filesystem::path findInjectorForArch(const std::filesystem::path &controllerDir, ProcessArch arch) {
    const auto siblingX86 = controllerDir.parent_path() / L"x86";
    const auto siblingX64 = controllerDir.parent_path() / L"x64";
    std::vector<std::filesystem::path> candidates;
    if (arch == ProcessArch::X86) {
        candidates = {
            controllerDir / L"x86" / L"krkr_injector.exe",
            siblingX86 / L"krkr_injector.exe",
            controllerDir / L"krkr_injector.exe"
        };
    } else if (arch == ProcessArch::X64) {
        candidates = {
            controllerDir / L"x64" / L"krkr_injector.exe",
            siblingX64 / L"krkr_injector.exe",
            controllerDir / L"krkr_injector.exe"
        };
    }
    for (const auto &c : candidates) {
        if (!c.empty() && std::filesystem::exists(c)) return c;
    }
    return {};
}

// timedOut: the injector is still running, so the hook may yet load; callers must not retry another path.
bool runInjector(const std::filesystem::path &injector, DWORD pid, const std::filesystem::path &dllPath,
                 std::wstring &error, bool &timedOut) {
    timedOut = false;
    if (injector.empty()) {
        error = L"Injector executable not found for target architecture.";
        return false;
    }

    std::wstring args = L"\"" + injector.wstring() + L"\" " + std::to_wstring(pid) + L" \"" + dllPath.wstring() + L"\"";
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(nullptr, args.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             injector.parent_path().c_str(), &si, &pi);
    if (!ok) {
        error = L"CreateProcess for injector failed: " + std::to_wstring(GetLastError());
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(pi.hProcess, kInjectorWaitMs);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (waitResult == WAIT_TIMEOUT || exitCode == STILL_ACTIVE) {
        timedOut = true;
        error = L"Injector timed out";
        return false;
    }
    if (exitCode == 8) {
        // Injector-side timeout: the remote LoadLibraryW is still pending.
        timedOut = true;
        error = L"Remote LoadLibraryW timed out";
        return false;
    }
    if (exitCode != 0) {
        error = L"Injector exit code " + std::to_wstring(exitCode);
        return false;
    }
    return true;
}

bool runInjectorElevated(const std::filesystem::path &injector, DWORD pid, const std::filesystem::path &dllPath,
                         std::wstring &error, bool &canceled, bool &timedOut) {
    canceled = false;
    timedOut = false;
    if (injector.empty()) {
        error = L"Injector executable not found for target architecture.";
        return false;
    }

    std::wstring params = std::to_wstring(pid) + L" \"" + dllPath.wstring() + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = injector.c_str();
    sei.lpParameters = params.c_str();
    const auto dir = injector.parent_path();
    if (!dir.empty()) {
        sei.lpDirectory = dir.c_str();
    }
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            canceled = true;
            error = L"Elevation canceled by user.";
        } else {
            error = L"Failed to elevate injector: " + std::to_wstring(err);
        }
        return false;
    }
    if (!sei.hProcess) {
        error = L"Elevated injector did not return a process handle.";
        return false;
    }
    DWORD waitResult = WaitForSingleObject(sei.hProcess, kInjectorWaitMs);
    DWORD exitCode = 0;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    if (waitResult == WAIT_TIMEOUT || exitCode == 8) {
        timedOut = true;
        error = L"Elevated injector timed out.";
        return false;
    }
    if (waitResult == WAIT_FAILED) {
        error = L"Elevated injector wait failed: " + std::to_wstring(GetLastError());
        return false;
    }
    if (exitCode != 0) {
        error = L"Elevated injector exit code " + std::to_wstring(exitCode);
        return false;
    }
    return true;
}

bool isProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = (elevation.TokenIsElevated != 0);
    }
    CloseHandle(token);
    return elevated;
}

std::vector<std::wstring> buildShortAndLongPaths(const std::filesystem::path &p) {
    std::vector<std::wstring> out;
    if (!p.empty()) out.push_back(p.wstring());
    wchar_t shortBuf[MAX_PATH] = {};
    DWORD shortLen = GetShortPathNameW(p.c_str(), shortBuf, static_cast<DWORD>(std::size(shortBuf)));
    if (shortLen > 0 && shortLen < std::size(shortBuf)) {
        out.emplace_back(shortBuf, shortLen);
    }
    return out;
}

// Keep shared setting mappings alive so the target process can open them after we return.
std::unordered_map<DWORD, HANDLE> g_sharedMappings;
std::mutex g_sharedMutex;

// Close mappings of processes that have exited. A process we may not open (e.g. elevated) counts as alive.
void pruneExitedMappingsLocked() {
    for (auto it = g_sharedMappings.begin(); it != g_sharedMappings.end();) {
        bool alive = true;
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, it->first);
        if (process) {
            alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            CloseHandle(process);
        } else {
            alive = GetLastError() != ERROR_INVALID_PARAMETER;
        }
        if (alive) {
            ++it;
            continue;
        }
        CloseHandle(it->second);
        it = g_sharedMappings.erase(it);
    }
}
} // namespace

ProcessArch getSelfArch() {
#if defined(_M_X64)
    return ProcessArch::X64;
#elif defined(_M_IX86)
    return ProcessArch::X86;
#elif defined(_M_ARM64)
    return ProcessArch::Arm64;
#else
    return ProcessArch::Unknown;
#endif
}

std::wstring describeArch(ProcessArch arch) {
    switch (arch) {
    case ProcessArch::X86: return L"x86";
    case ProcessArch::X64: return L"x64";
    case ProcessArch::Arm64: return L"ARM64";
    default: return L"unknown";
    }
}


float clampSpeed(float speed) {
    return std::clamp(speed, 0.5f, 10.0f);
}

float roundSpeed(float speed) {
    return std::round(speed * 100.0f) / 100.0f;
}

float effectiveSpeed(const SpeedControlState &state) {
    return state.enabled ? state.currentSpeed : 1.0f;
}

std::wstring formatSpeed(float speed) {
    wchar_t buf[32] = {};
    swprintf_s(buf, L"%.2f", speed);
    return std::wstring(buf);
}

void initSpeedState(SpeedControlState &state, float speed, bool enabled) {
    state.enabled = enabled;
    const float clamped = clampSpeed(speed);
    state.currentSpeed = clamped;
    state.lastValidSpeed = clamped;
}

void updateSpeedFromInput(SpeedControlState &state, float speed) {
    const float clamped = clampSpeed(speed);
    state.currentSpeed = clamped;
    state.lastValidSpeed = clamped;
}

bool applySpeedToPid(DWORD pid, const SharedConfig &baseConfig, const SpeedControlState &state, std::wstring &error) {
    if (pid == 0) {
        error = L"Invalid PID";
        return false;
    }
    SharedConfig cfg = baseConfig;
    cfg.speed = effectiveSpeed(state);
    return writeSharedSettingsForPid(pid, cfg, error);
}

std::wstring applySpeedHotkey(SpeedControlState &state, SpeedHotkeyAction action) {
    auto setSpeed = [&](float speed) {
        updateSpeedFromInput(state, roundSpeed(speed));
    };

    switch (action) {
    case SpeedHotkeyAction::Toggle:
        state.enabled = !state.enabled;
        if (state.enabled) {
            return L"Speed ON: " + formatSpeed(state.currentSpeed) + L"x";
        }
        return L"Speed OFF (target " + formatSpeed(state.currentSpeed) + L"x)";
    case SpeedHotkeyAction::SpeedUp:
        if (!state.enabled) {
            state.enabled = true;
            setSpeed(1.1f);
        } else {
            setSpeed(state.currentSpeed + 0.1f);
        }
        break;
    case SpeedHotkeyAction::SpeedDown:
        if (!state.enabled) {
            state.enabled = true;
            setSpeed(0.9f);
        } else {
            setSpeed(state.currentSpeed - 0.1f);
        }
        break;
    }
    return L"Speed set to " + formatSpeed(state.currentSpeed) + L"x";
}

bool ensureDebugPrivilege() {
    static bool attempted = false;
    static bool enabled = false;
    if (attempted) return enabled;
    attempted = true;

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        KRKR_LOG_WARN("OpenProcessToken failed while enabling SeDebugPrivilege: " + std::to_string(GetLastError()));
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        KRKR_LOG_WARN("LookupPrivilegeValueW(SE_DEBUG_NAME) failed: " + std::to_string(GetLastError()));
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        KRKR_LOG_WARN("AdjustTokenPrivileges failed: " + std::to_string(GetLastError()));
        CloseHandle(token);
        return false;
    }
    const DWORD lastErr = GetLastError();
    CloseHandle(token);
    if (lastErr == ERROR_NOT_ALL_ASSIGNED) {
        KRKR_LOG_WARN("SeDebugPrivilege not assigned; elevated/protected targets may still reject injection");
        return false;
    }

    enabled = true;
    KRKR_LOG_INFO("SeDebugPrivilege enabled for injector process");
    return true;
}

std::filesystem::path controllerDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        return {};
    }
    std::filesystem::path path(buffer);
    return path.parent_path();
}

std::vector<ProcessInfo> enumerateVisibleProcesses() {
    std::vector<ProcessInfo> result;

    DWORD currentSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) {
        KRKR_LOG_WARN("ProcessIdToSessionId failed for current process; defaulting to no session filter");
        currentSession = (std::numeric_limits<DWORD>::max)();
    }

    detail::refreshProcessBlacklist();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        KRKR_LOG_ERROR("CreateToolhelp32Snapshot failed: " + std::to_string(GetLastError()));
        return result;
    }
    const auto windowTitles = collectVisibleWindowTitles();

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session)) {
                continue;
            }
            if (session == 0 || session != currentSession) {
                continue; // skip services and other sessions
            }
            if (isProcessBlacklisted(entry.szExeFile)) {
                continue;
            }
            const auto titleIt = windowTitles.find(entry.th32ProcessID);
            if (titleIt == windowTitles.end()) {
                continue; // skip helpers/subprocesses without a top-level window
            }

            ProcessInfo info;
            info.name = entry.szExeFile;
            info.windowTitle = titleIt->second;
            info.pid = entry.th32ProcessID;
            info.hasWindow = true;
            result.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    } else {
        KRKR_LOG_ERROR("Process32FirstW failed: " + std::to_string(GetLastError()));
    }

    CloseHandle(snapshot);
    return result;
}

std::vector<ProcessInfo> enumerateSessionProcesses() {
    std::vector<ProcessInfo> result;

    DWORD currentSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) {
        KRKR_LOG_WARN("ProcessIdToSessionId failed for current process; defaulting to no session filter");
        currentSession = (std::numeric_limits<DWORD>::max)();
    }

    detail::refreshProcessBlacklist();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        KRKR_LOG_ERROR("CreateToolhelp32Snapshot failed: " + std::to_string(GetLastError()));
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session)) {
                continue;
            }
            if (session == 0 || session != currentSession) {
                continue; // skip services and other sessions
            }
            if (isProcessBlacklisted(entry.szExeFile)) {
                continue;
            }

            ProcessInfo info;
            info.name = entry.szExeFile;
            info.pid = entry.th32ProcessID;
            info.hasWindow = false;
            result.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    } else {
        KRKR_LOG_ERROR("Process32FirstW failed: " + std::to_string(GetLastError()));
    }

    CloseHandle(snapshot);
    std::sort(result.begin(), result.end(), [](const ProcessInfo &a, const ProcessInfo &b) {
        return a.name < b.name;
    });
    return result;
}

bool queryProcessArch(DWORD pid, ProcessArch &archOut, std::wstring &error) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        error = L"OpenProcess failed while probing architecture (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using IsWow64Process2Fn = BOOL(WINAPI *)(HANDLE, USHORT *, USHORT *);
    auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(kernel, "IsWow64Process2"));
    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            archOut = classifyMachine(processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine);
            CloseHandle(process);
            return true;
        }
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64)) {
        SYSTEM_INFO info{};
        GetNativeSystemInfo(&info);
        if (wow64) {
            archOut = ProcessArch::X86;
        } else {
            switch (info.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64: archOut = ProcessArch::X64; break;
            case PROCESSOR_ARCHITECTURE_ARM64: archOut = ProcessArch::Arm64; break;
            default: archOut = ProcessArch::Unknown; break;
            }
        }
        CloseHandle(process);
        return true;
    }

    error = L"Unable to query process architecture (error " + std::to_wstring(GetLastError()) + L")";
    CloseHandle(process);
    return false;
}

bool getProcessExePath(DWORD pid, std::wstring &pathOut, std::wstring &error) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        error = L"OpenProcess failed while reading path (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (QueryFullProcessImageNameW(process, 0, buffer, &size) == 0) {
        CloseHandle(process);
        error = L"QueryFullProcessImageNameW failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    CloseHandle(process);
    pathOut.assign(buffer, size);
    return true;
}

bool getDllArch(const std::filesystem::path &path, ProcessArch &archOut, std::wstring &error) {
    archOut = ProcessArch::Unknown;
    if (!std::filesystem::exists(path)) {
        error = L"Hook DLL not found: " + path.wstring();
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = L"Unable to open DLL to read architecture: " + path.wstring();
        return false;
    }

    std::uint8_t header[0x44] = {};
    file.read(reinterpret_cast<char *>(header), sizeof(header));
    if (file.gcount() < 0x44) {
        error = L"Unable to read DLL headers for: " + path.wstring();
        return false;
    }
    const std::uint32_t peOffset = *reinterpret_cast<const std::uint32_t *>(header + 0x3c);
    file.seekg(peOffset + 4, std::ios::beg);
    std::uint16_t machine = 0;
    file.read(reinterpret_cast<char *>(&machine), sizeof(machine));
    if (file.gcount() != sizeof(machine)) {
        error = L"Unable to read DLL machine type: " + path.wstring();
        return false;
    }

    archOut = classifyMachine(machine);
    return true;
}

bool selectHookForArch(const std::filesystem::path &controllerDir, ProcessArch arch, std::filesystem::path &outPath,
                       std::wstring &error) {
    const auto siblingX86 = controllerDir.parent_path() / L"x86";
    const auto siblingX64 = controllerDir.parent_path() / L"x64";
    std::vector<std::filesystem::path> candidates;
    if (arch == ProcessArch::X86) {
        candidates = {
            controllerDir / L"x86" / L"krkr_speed_hook.dll",
            siblingX86 / L"krkr_speed_hook.dll",
            controllerDir / L"krkr_speed_hook32.dll",
            controllerDir / L"krkr_speed_hook_x86.dll",
            controllerDir / L"krkr_speed_hook.dll"
        };
    } else {
        candidates = {
            controllerDir / L"x64" / L"krkr_speed_hook.dll",
            siblingX64 / L"krkr_speed_hook.dll",
            controllerDir / L"krkr_speed_hook64.dll",
            controllerDir / L"krkr_speed_hook_x64.dll",
            controllerDir / L"krkr_speed_hook.dll"
        };
    }

    for (const auto &candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            outPath = std::filesystem::absolute(candidate);
            return true;
        }
    }

    if (arch == ProcessArch::X86) {
        error = L"Matching hook DLL not found. Place x86/krkr_speed_hook.dll (or krkr_speed_hook32.dll) next to the controller.";
    } else {
        error = L"Matching hook DLL not found. Place x64/krkr_speed_hook.dll (or krkr_speed_hook64.dll) next to the controller.";
    }
    return false;
}

bool writeSharedSettingsForPid(DWORD pid, const SharedConfig &config, std::wstring &error) {
    const auto name = BuildSharedSettingsName(static_cast<std::uint32_t>(pid));

    HANDLE mapping = nullptr;
    auto recreateMapping = [&]() -> HANDLE {
        return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedSettings), name.c_str());
    };

    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        auto it = g_sharedMappings.find(pid);
        if (it != g_sharedMappings.end()) {
            mapping = it->second;
        }
    }
    if (!mapping) {
        mapping = recreateMapping();
    }
    if (!mapping) {
        error = L"CreateFileMapping failed: " + std::to_wstring(GetLastError());
        return false;
    }

    auto mapView = [&](HANDLE h) -> SharedSettings * {
        // Size 0 maps the whole section: one created by an older controller may be shorter than ours, but
        // views are page-granular, so the full struct is still addressable.
        return static_cast<SharedSettings *>(MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, 0));
    };

    SharedSettings *view = mapView(mapping);
    if (!view && GetLastError() == ERROR_INVALID_HANDLE) {
        // Handle likely closed/invalid; recreate once.
        mapping = recreateMapping();
        if (mapping) {
            view = mapView(mapping);
        }
    }
    if (!view) {
        error = L"MapViewOfFile failed: " + std::to_wstring(GetLastError());
        return false;
    }

    SharedSettings settings{};
    settings.userSpeed = config.speed;
    settings.lengthGateSeconds = std::clamp(config.bgmSeconds, 0.1f, 600.0f);
    settings.lengthGateEnabled = config.lengthGateEnabled ? 1u : 0u;
    settings.enableLog = config.enableLog ? 1u : 0u;
    settings.disableBgm = 0;
    settings.processAllAudio = config.processAllAudio ? 1u : 0u;
    settings.bgmSecondsGate = std::clamp(config.bgmSeconds, 0.1f, 600.0f);
    settings.stereoBgmMode = config.stereoBgmMode;
    settings.layoutSize = static_cast<std::uint32_t>(sizeof(SharedSettings));
    settings.controllerVersion = KRKR_VERSION_PACKED;
    *view = settings;

    UnmapViewOfFile(view);
    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        auto it = g_sharedMappings.find(pid);
        if (it != g_sharedMappings.end() && it->second != mapping) {
            CloseHandle(it->second);
        }
        g_sharedMappings[pid] = mapping; // keep alive for hook to open
        pruneExitedMappingsLocked();
    }
    return true;
}

bool injectDllIntoProcess(ProcessArch targetArch, DWORD pid, const std::filesystem::path &dllPath, std::wstring &error) {
    ensureDebugPrivilege();

    const ProcessArch selfArch = getSelfArch();
    const bool archMismatch = (targetArch != ProcessArch::Unknown && selfArch != ProcessArch::Unknown && targetArch != selfArch);
    if (!archMismatch) {
        auto preflightLoad = [](const std::filesystem::path &path, std::wstring &err) -> bool {
            // The skip flag is process-wide; serialize so a concurrent (auto-hook) preflight cannot
            // clear it while another thread's LoadLibraryW runs the hook's DllMain in the controller.
            static std::mutex preflightMutex;
            std::lock_guard<std::mutex> lock(preflightMutex);
            SetEnvironmentVariableW(L"KRKR_SKIP_HOOK_INIT", L"1");
            HMODULE localHandle = LoadLibraryW(path.c_str());
            SetEnvironmentVariableW(L"KRKR_SKIP_HOOK_INIT", nullptr);
            if (!localHandle) {
                err = L"Preflight LoadLibraryW failed locally (error " + std::to_wstring(GetLastError()) +
                      L"); check dependencies beside the DLL.";
                return false;
            }
            FreeLibrary(localHandle);
            return true;
        };

        std::wstring preflightErr;
        if (!preflightLoad(dllPath, preflightErr)) {
            error = preflightErr;
            return false;
        }
    } else {
        KRKR_LOG_INFO("Skipping preflight load due to arch mismatch controller=" + std::string(selfArch == ProcessArch::X64 ? "x64" : "x86")
                      + " target=" + std::string(targetArch == ProcessArch::X64 ? "x64" : "x86"));
    }

    std::vector<std::wstring> attemptNotes;
    std::vector<std::wstring> candidatePaths;
    std::wstring lastError;
    const auto controllerDir = controllerDirectory();
    const auto injector = findInjectorForArch(controllerDir, targetArch);
    auto addCandidate = [&](const std::wstring &p) {
        if (p.empty()) return;
        if (std::find(candidatePaths.begin(), candidatePaths.end(), p) == candidatePaths.end()) {
            candidatePaths.push_back(p);
        }
    };
    const auto timedOutError = [&](const std::wstring &path) {
        // The hook may still finish loading; trying another path could load a second copy.
        return L"Injection timed out for " + path + L"; the hook may still load. Not retrying other paths.";
    };

    std::size_t tried = 0;
    auto tryCandidates = [&](bool &timedOut) -> bool {
        timedOut = false;
        for (; tried < candidatePaths.size(); ++tried) {
            const auto &p = candidatePaths[tried];
            if (runInjector(injector, pid, p, lastError, timedOut)) {
                return true;
            }
            attemptNotes.push_back(L"#" + std::to_wstring(tried) + L" " + p + L": " + lastError);
            if (timedOut) {
                return false;
            }
        }
        return false;
    };

    // 1) Load the hook from the controller folder; SoundTouch.dll is delay-loaded from beside it.
    for (const auto &p : buildShortAndLongPaths(dllPath)) {
        addCandidate(p);
    }
    bool timedOut = false;
    if (tryCandidates(timedOut)) {
        return true;
    }
    if (timedOut) {
        error = timedOutError(candidatePaths[tried]);
        return false;
    }

    // 2) Fallback: copy the hook beside the game executable. An existing SoundTouch.dll there is kept.
    const auto targetDir = getProcessDirectory(pid);
    if (!targetDir.empty() && targetDir != dllPath.parent_path()) {
        std::error_code ec;
        const auto targetDll = targetDir / dllPath.filename();
        std::filesystem::copy_file(dllPath, targetDll, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            attemptNotes.push_back(L"Copy to target dir failed for hook: " + targetDll.wstring() +
                                   L" (error " + std::to_wstring(ec.value()) + L")");
        } else {
            KRKR_LOG_INFO(L"Injection fallback: copied hook to " + targetDll.wstring());
            const auto dep = dllPath.parent_path() / L"SoundTouch.dll";
            const auto targetDep = targetDir / dep.filename();
            if (std::filesystem::exists(dep, ec) && !std::filesystem::exists(targetDep, ec)) {
                std::filesystem::copy_file(dep, targetDep, std::filesystem::copy_options::skip_existing, ec);
                if (ec) {
                    attemptNotes.push_back(L"Copy to target dir failed for SoundTouch: " + targetDep.wstring() +
                                           L" (error " + std::to_wstring(ec.value()) + L")");
                }
            }
            for (const auto &p : buildShortAndLongPaths(targetDll)) {
                addCandidate(p);
            }
            if (tryCandidates(timedOut)) {
                return true;
            }
            if (timedOut) {
                error = timedOutError(candidatePaths[tried]);
                return false;
            }
        }
    }

    // 3) Elevated retry for protected/elevated targets.
    if (!isProcessElevated()) {
        KRKR_LOG_INFO("Injector failed; retrying with elevated injector");
        bool canceled = false;
        for (const auto &p : candidatePaths) {
            if (runInjectorElevated(injector, pid, p, lastError, canceled, timedOut)) {
                return true;
            }
            if (canceled) {
                error = lastError;
                return false;
            }
            if (timedOut) {
                error = timedOutError(p);
                return false;
            }
        }
        error = L"Elevated injector failed: " + lastError;
        return false;
    }

    error = L"DLL injection returned 0 (remote LoadLibraryW failed). ";
    if (!attemptNotes.empty()) {
        error += L"Attempts: ";
        for (size_t i = 0; i < attemptNotes.size(); ++i) {
            if (i) error += L" | ";
            error += attemptNotes[i];
        }
    } else {
        error += L"Tried path: " + dllPath.wstring();
    }
    if (!targetDir.empty()) {
        error += L"; target dir: " + targetDir.wstring();
    }
    if (!lastError.empty()) {
        error += L" Last attempt: " + lastError;
    }
    return false;
}

AudioBackend getAudioBackendForPid(DWORD pid) {
    if (pid == 0) {
        return AudioBackend::Unknown;
    }
    const auto name = BuildSharedStatusName(static_cast<std::uint32_t>(pid));
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) {
        return AudioBackend::Unknown;
    }
    auto *view = static_cast<SharedStatus *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedStatus)));
    if (!view) {
        CloseHandle(mapping);
        return AudioBackend::Unknown;
    }
    const auto backend = static_cast<AudioBackend>(view->activeBackend);
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return backend;
}

bool isWasapiActiveForPid(DWORD pid) {
    return getAudioBackendForPid(pid) == AudioBackend::Wasapi;
}

bool launchAndInject(const std::filesystem::path &exePath, const SharedConfig &config, DWORD &outPid, std::wstring &error) {
    if (exePath.empty() || !std::filesystem::exists(exePath)) {
        error = L"Invalid game path";
        return false;
    }
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    std::wstring cmd = L"\"" + exePath.wstring() + L"\"";
    auto workDir = exePath.parent_path();
    BOOL ok = CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(),
                             &si, &pi);
    if (!ok) {
        error = L"CreateProcess failed: " + std::to_wstring(GetLastError());
        return false;
    }

    outPid = pi.dwProcessId;

    ProcessArch targetArch = ProcessArch::Unknown;
    std::wstring archError;
    if (!queryProcessArch(pi.dwProcessId, targetArch, archError)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = archError;
        return false;
    }

    std::wstring sharedErr;
    if (!writeSharedSettingsForPid(pi.dwProcessId, config, sharedErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = sharedErr;
        return false;
    }

    const auto baseDir = controllerDirectory();
    std::filesystem::path dllPath;
    std::wstring archErr;
    if (!selectHookForArch(baseDir, targetArch, dllPath, archErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = archErr;
        return false;
    }

    ProcessArch dllArch = ProcessArch::Unknown;
    std::wstring dllArchError;
    if (!getDllArch(dllPath, dllArch, dllArchError) || (dllArch != ProcessArch::Unknown && dllArch != targetArch)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = L"Hook DLL arch mismatch";
        return false;
    }

    std::wstring injectErr;
    if (!injectDllIntoProcess(targetArch, pi.dwProcessId, dllPath, injectErr)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = L"Launch inject failed: " + injectErr;
        return false;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace krkrspeed::controller
