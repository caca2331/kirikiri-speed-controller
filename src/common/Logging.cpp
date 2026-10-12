#include "Logging.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <optional>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace krkrspeed {

namespace {

struct LoggerState {
    std::mutex mutex;
    std::ofstream stream;
    bool initialized = false;
    std::atomic<bool> enabled{false};
    std::optional<std::filesystem::path> logDirOverride;
};

std::string levelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNK";
}

std::string currentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string toUtf8(const std::wstring &wstr) {
#ifdef _WIN32
    if (wstr.empty()) return {};
    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string result(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
#else
    // Narrowing best-effort for non-Windows builds.
    std::string result;
    result.reserve(wstr.size());
    for (wchar_t ch : wstr) {
        result.push_back(static_cast<char>(ch));
    }
    return result;
#endif
}

unsigned long currentProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

LoggerState &state() {
    static LoggerState instance;
    return instance;
}

// Set while a thread is inside logMessage; re-entrant calls (e.g. from the hook's vectored
// exception handler reacting to OutputDebugString) are dropped instead of re-locking the mutex.
thread_local bool t_inLogger = false;

// Path of the module that contains this code: the hook DLL inside a game, the exe in the controller.
std::filesystem::path currentModulePath() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&currentModulePath), &module)) {
        module = nullptr;
    }
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(module, buffer, MAX_PATH) == 0) {
        return {};
    }
    return std::filesystem::path(buffer);
#else
    return {};
#endif
}

std::filesystem::path moduleDirectory() {
    return currentModulePath().parent_path();
}

// Controller-side cleanup of logs from previous sessions. Logs still held open by a running
// hooked game cannot be deleted (no FILE_SHARE_DELETE) and are left alone.
void pruneOldLogs(const std::filesystem::path &dir) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) continue;
        const auto name = entry.path().filename().wstring();
        const bool known = name == L"krkr_speed.log" || name == L"krkr_controller.log" || name == L"krkr_hook.log" ||
                           (name.rfind(L"krkr_hook_", 0) == 0 && entry.path().extension() == L".log");
        if (known) {
            std::filesystem::remove(entry.path(), entryEc);
        }
    }
}

std::filesystem::path readHintPath() {
    std::error_code ec;
    auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    auto hintFile = temp / "krkr_log_dir.txt";
    if (!std::filesystem::exists(hintFile, ec)) return {};
    std::ifstream in(hintFile);
    if (!in) return {};
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    if (line.empty()) return {};
    // The controller writes the hint as UTF-8; a plain std::string path would be read as ANSI.
    std::filesystem::path p = std::filesystem::u8path(line);
    if (std::filesystem::is_directory(p, ec)) {
        return p;
    }
    return {};
}

std::filesystem::path chooseLogDirectory() {
    auto &s = state();
    std::error_code ec;
    if (s.logDirOverride && std::filesystem::is_directory(*s.logDirOverride, ec)) {
        return *s.logDirOverride;
    }
    // 1) hint file in temp
    auto hint = readHintPath();
    if (!hint.empty()) return hint;
    // 2) module directory
    auto mod = moduleDirectory();
    if (!mod.empty()) return mod;
    // 3) temp
    return std::filesystem::temp_directory_path(ec);
}

void ensureOpen(LoggerState &stateRef) {
    if (stateRef.initialized) {
        return;
    }
    stateRef.initialized = true;

    auto dir = chooseLogDirectory();
    std::error_code ec;

    const auto stem = currentModulePath().stem().wstring();
    std::string base = "krkr_speed";
    if (stem == L"KrkrSpeedController") {
        base = "krkr_controller";
        pruneOldLogs(dir);
    } else if (stem == L"krkr_speed_hook") {
        // One log per hooked process so several games never truncate each other's log.
        base = "krkr_hook_" + std::to_string(currentProcessId());
    }

    const auto path = dir / (base + ".log");
    std::filesystem::remove(path, ec);

    stateRef.stream.open(path, std::ios::out | std::ios::trunc);
    if (stateRef.stream.is_open()) {
        stateRef.stream << "----- log start " << currentTimestamp() << " (pid " << currentProcessId() << ") -----"
                        << std::endl;
    }
}

void writeLine(LoggerState &stateRef, LogLevel level, const std::string &line) {
    if (!stateRef.stream.is_open()) {
        return;
    }
    stateRef.stream << "[" << currentTimestamp() << "] [" << levelToString(level) << "] " << line << std::endl;
}

} // namespace

void logMessage(LogLevel level, const std::string &message) {
    auto &s = state();
    if (!s.enabled.load(std::memory_order_relaxed) || t_inLogger) {
        return;
    }
    struct ReentryGuard {
        ReentryGuard() { t_inLogger = true; }
        ~ReentryGuard() { t_inLogger = false; }
    } guard;

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        ensureOpen(s);
        if (s.stream.is_open()) {
            writeLine(s, level, message);
        } else {
            // Fallback to stderr if the log file cannot be opened.
            std::clog << "[" << levelToString(level) << "] " << message << std::endl;
        }
    }
#ifdef _WIN32
    // Outside the lock: OutputDebugString raises a (handled) exception that vectored handlers see.
    const std::string dbg = "[krkr] " + message + "\n";
    OutputDebugStringA(dbg.c_str());
#endif
}

void logMessage(LogLevel level, const std::wstring &message) {
    logMessage(level, toUtf8(message));
}

void SetLoggingEnabled(bool enabled) {
    auto &s = state();
    s.enabled.store(enabled);
}

void SetLogDirectory(const std::wstring &path) {
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (path.empty()) {
        s.logDirOverride.reset();
        return;
    }
    s.logDirOverride = std::filesystem::path(path);
}

} // namespace krkrspeed
