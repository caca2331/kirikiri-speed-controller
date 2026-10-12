#include "ControllerCore.h"
#include "ControllerInternal.h"
#include "../common/Logging.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace krkrspeed::controller {
namespace {

std::vector<AutoHookEntry> g_autoHookEntries;
std::vector<AutoHookEntry> g_processBgmEntries;
std::mutex g_autoHookMutex;
bool g_minimizeToTrayOnClose = false;
std::unordered_set<std::wstring> g_processBlacklist;
std::filesystem::file_time_type g_processBlacklistStamp{};
std::mutex g_processBlacklistMutex;
bool g_processBlacklistLoaded = false;
bool g_loggedBlacklistMissing = false;

constexpr wchar_t kAutoHookConfigName[] = L"krkr_speed_config.yaml";
constexpr wchar_t kProcessBlacklistName[] = L"process_blacklist.txt";

} // namespace

namespace detail {

std::string toUtf8(const std::wstring &wstr) {
    if (wstr.empty()) return {};
    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string result(static_cast<size_t>(sizeNeeded), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

std::wstring fromUtf8(const std::string &str) {
    if (str.empty()) return {};
    const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (sizeNeeded <= 0) return {};
    std::wstring result(static_cast<size_t>(sizeNeeded), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), sizeNeeded);
    return result;
}

std::string trimCopy(const std::string &s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::wstring toLowerCopy(const std::wstring &input) {
    std::wstring out;
    out.reserve(input.size());
    for (wchar_t c : input) {
        out.push_back(static_cast<wchar_t>(std::towlower(c)));
    }
    return out;
}

std::filesystem::path processBlacklistPath() {
    const auto dir = controllerDirectory();
    if (dir.empty()) return {};
    return dir / kProcessBlacklistName;
}

void refreshProcessBlacklist() {
    std::lock_guard<std::mutex> lock(g_processBlacklistMutex);
    const auto path = processBlacklistPath();
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    const auto stamp = exists ? std::filesystem::last_write_time(path, ec) : std::filesystem::file_time_type{};
    if (g_processBlacklistLoaded && !ec && stamp == g_processBlacklistStamp) {
        return;
    }
    g_processBlacklist.clear();
    g_processBlacklistStamp = stamp;
    g_processBlacklistLoaded = true;
    if (!exists || ec) {
        if (!g_loggedBlacklistMissing) {
            g_loggedBlacklistMissing = true;
            KRKR_LOG_INFO("Process blacklist not found; skipping filter (" + toUtf8(path.wstring()) + ")");
        }
        return;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        KRKR_LOG_WARN("Failed to open process blacklist: " + toUtf8(path.wstring()));
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trimCopy(line);
        if (trimmed.empty()) continue;
        if (trimmed.rfind("//", 0) == 0) continue;
        if (!trimmed.empty() && (trimmed[0] == '#' || trimmed[0] == ';')) continue;
        std::wstring wline = fromUtf8(trimmed);
        if (wline.empty()) continue;
        std::filesystem::path p(wline);
        const auto name = p.filename().wstring();
        const auto key = toLowerCopy(name.empty() ? wline : name);
        if (!key.empty()) {
            g_processBlacklist.insert(key);
        }
    }
    KRKR_LOG_INFO("Process blacklist loaded (" + std::to_string(g_processBlacklist.size()) + " entries)");
}

bool isProcessBlacklisted(const std::wstring &exeName) {
    std::lock_guard<std::mutex> lock(g_processBlacklistMutex);
    if (g_processBlacklist.empty()) return false;
    const auto key = toLowerCopy(exeName);
    return g_processBlacklist.find(key) != g_processBlacklist.end();
}

} // namespace detail

namespace {
using detail::fromUtf8;
using detail::toUtf8;
using detail::trimCopy;

std::string unescapeYamlString(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    bool escape = false;
    for (char c : s) {
        if (escape) {
            out.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::string escapeYamlString(const std::wstring &value) {
    std::string utf8 = toUtf8(value);
    std::string out;
    out.reserve(utf8.size());
    for (char c : utf8) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string formatDelaySeconds(double seconds) {
    if (!std::isfinite(seconds)) {
        return "0.0";
    }
    const double rounded = std::round(seconds);
    std::ostringstream out;
    if (std::fabs(seconds - rounded) < 0.0001) {
        out.setf(std::ios::fixed);
        out << std::setprecision(1) << seconds;
    } else {
        out.setf(std::ios::fixed);
        out << std::setprecision(3) << seconds;
    }
    return out.str();
}

bool matchAutoHookEntry(const AutoHookEntry &entry, const std::wstring &exePath, const std::wstring &exeName) {
    return _wcsicmp(entry.exePath.c_str(), exePath.c_str()) == 0 &&
           _wcsicmp(entry.exeName.c_str(), exeName.c_str()) == 0;
}

std::filesystem::path autoHookConfigPath() {
    const auto dir = controllerDirectory();
    if (dir.empty()) return {};
    return dir / kAutoHookConfigName;
}

bool saveAutoHookConfig(std::wstring &error) {
    const auto path = autoHookConfigPath();
    if (path.empty()) {
        error = L"Unable to resolve controller directory for config.";
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = L"Unable to write config: " + path.wstring();
        return false;
    }
    out << "controller:\n";
    out << "  minimize_to_tray_on_close: " << (g_minimizeToTrayOnClose ? "true" : "false") << "\n";
    out << "auto_hook:\n";
    for (const auto &entry : g_autoHookEntries) {
        out << "  - name: \"" << escapeYamlString(entry.exeName) << "\"\n";
        out << "    path: \"" << escapeYamlString(entry.exePath) << "\"\n";
        if (entry.hasDelay && entry.delaySeconds > 0.0) {
            out << "    delay: " << formatDelaySeconds(entry.delaySeconds) << "\n";
        }
    }
    out << "process_bgm:\n";
    for (const auto &entry : g_processBgmEntries) {
        out << "  - name: \"" << escapeYamlString(entry.exeName) << "\"\n";
        out << "    path: \"" << escapeYamlString(entry.exePath) << "\"\n";
    }
    return true;
}
} // namespace

void loadAutoHookConfig() {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    g_autoHookEntries.clear();
    g_processBgmEntries.clear();
    g_minimizeToTrayOnClose = false;
    const auto path = autoHookConfigPath();
    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string contents = buffer.str();
    std::istringstream lines(contents);

    enum class Section {
        None,
        AutoHook,
        ProcessBgm,
        Controller
    };
    Section section = Section::None;
    std::vector<AutoHookEntry> *activeList = nullptr;
    AutoHookEntry current{};
    bool hasName = false;
    bool hasPath = false;

    auto flush = [&]() {
        if (activeList && hasName && hasPath) {
            activeList->push_back(current);
        }
        current = AutoHookEntry{};
        hasName = false;
        hasPath = false;
    };

    std::string line;
    while (std::getline(lines, line)) {
        std::size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) {
            ++indent;
        }
        std::string trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (indent == 0 && trimmed.back() == ':') {
            const std::string key = trimCopy(trimmed.substr(0, trimmed.size() - 1));
            if (section == Section::AutoHook || section == Section::ProcessBgm) {
                flush();
            }
            if (key == "auto_hook") {
                section = Section::AutoHook;
                activeList = &g_autoHookEntries;
            } else if (key == "process_bgm") {
                section = Section::ProcessBgm;
                activeList = &g_processBgmEntries;
            } else if (key == "controller") {
                section = Section::Controller;
                activeList = nullptr;
            } else {
                section = Section::None;
                activeList = nullptr;
            }
            continue;
        }

        if (section == Section::None) {
            continue;
        }

        if (section == Section::Controller) {
            const auto colon = trimmed.find(':');
            if (colon == std::string::npos) continue;
            std::string key = trimCopy(trimmed.substr(0, colon));
            std::string value = trimCopy(trimmed.substr(colon + 1));
            if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.size() - 2);
            }
            std::string lower;
            lower.reserve(value.size());
            for (char c : value) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            if (key == "minimize_to_tray_on_close") {
                g_minimizeToTrayOnClose = (lower == "true" || lower == "1" || lower == "yes" || lower == "on");
            }
            continue;
        }

        if (trimmed[0] == '-') {
            flush();
            trimmed = trimCopy(trimmed.substr(1));
            if (trimmed.empty()) continue;
        }
        const auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trimCopy(trimmed.substr(0, colon));
        std::string value = trimCopy(trimmed.substr(colon + 1));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (key == "name") {
            current.exeName = fromUtf8(unescapeYamlString(value));
            hasName = !current.exeName.empty();
        } else if (key == "path") {
            current.exePath = fromUtf8(unescapeYamlString(value));
            hasPath = !current.exePath.empty();
        } else if (key == "delay" && activeList == &g_autoHookEntries) {
            try {
                const double parsed = std::stod(value);
                if (parsed > 0.0) {
                    current.delaySeconds = parsed;
                    current.hasDelay = true;
                } else {
                    current.delaySeconds = 0.0;
                    current.hasDelay = false;
                }
            } catch (...) {
                // Ignore malformed delay values.
            }
        }
    }
    if (section == Section::AutoHook || section == Section::ProcessBgm) {
        flush();
    }
}

bool isAutoHookEnabled(const std::wstring &exePath, const std::wstring &exeName) {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    for (const auto &entry : g_autoHookEntries) {
        if (matchAutoHookEntry(entry, exePath, exeName)) {
            return true;
        }
    }
    return false;
}

bool setAutoHookEnabled(const std::wstring &exePath, const std::wstring &exeName, bool enabled, std::wstring &error) {
    if (exePath.empty() || exeName.empty()) {
        error = L"Invalid exe path/name.";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    auto it = std::remove_if(g_autoHookEntries.begin(), g_autoHookEntries.end(),
                             [&](const AutoHookEntry &entry) { return matchAutoHookEntry(entry, exePath, exeName); });
    g_autoHookEntries.erase(it, g_autoHookEntries.end());
    if (enabled) {
        g_autoHookEntries.push_back(AutoHookEntry{exeName, exePath});
    }
    return saveAutoHookConfig(error);
}

std::size_t autoHookEntryCount() {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    return g_autoHookEntries.size();
}

std::vector<std::wstring> autoHookExeNames() {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    std::vector<std::wstring> names;
    names.reserve(g_autoHookEntries.size());
    for (const auto &entry : g_autoHookEntries) {
        if (!entry.exeName.empty()) {
            names.push_back(entry.exeName);
        }
    }
    return names;
}

bool tryGetAutoHookDelay(const std::wstring &exePath, const std::wstring &exeName, double &outDelaySeconds) {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    for (const auto &entry : g_autoHookEntries) {
        if (matchAutoHookEntry(entry, exePath, exeName) && entry.hasDelay && entry.delaySeconds > 0.0) {
            outDelaySeconds = entry.delaySeconds;
            return true;
        }
    }
    return false;
}

bool setAutoHookDelay(const std::wstring &exePath, const std::wstring &exeName, bool enabled, double delaySeconds,
                      std::wstring &error) {
    if (exePath.empty() || exeName.empty()) {
        error = L"Invalid exe path/name.";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    auto it = std::find_if(g_autoHookEntries.begin(), g_autoHookEntries.end(),
                           [&](const AutoHookEntry &entry) { return matchAutoHookEntry(entry, exePath, exeName); });
    if (it == g_autoHookEntries.end()) {
        if (!enabled) {
            return true;
        }
        AutoHookEntry entry{exeName, exePath};
        if (enabled && delaySeconds > 0.0) {
            entry.hasDelay = true;
            entry.delaySeconds = delaySeconds;
        }
        g_autoHookEntries.push_back(std::move(entry));
    } else {
        if (enabled && delaySeconds > 0.0) {
            it->hasDelay = true;
            it->delaySeconds = delaySeconds;
        } else {
            it->hasDelay = false;
            it->delaySeconds = 0.0;
        }
    }
    return saveAutoHookConfig(error);
}

bool getMinimizeToTrayOnClose() {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    return g_minimizeToTrayOnClose;
}

bool setMinimizeToTrayOnClose(bool enabled, std::wstring &error) {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    g_minimizeToTrayOnClose = enabled;
    return saveAutoHookConfig(error);
}

bool isProcessBgmEnabled(const std::wstring &exePath, const std::wstring &exeName) {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    for (const auto &entry : g_processBgmEntries) {
        if (matchAutoHookEntry(entry, exePath, exeName)) {
            return true;
        }
    }
    return false;
}

bool setProcessBgmEnabled(const std::wstring &exePath, const std::wstring &exeName, bool enabled, std::wstring &error) {
    if (exePath.empty() || exeName.empty()) {
        error = L"Invalid exe path/name.";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    auto it = std::remove_if(g_processBgmEntries.begin(), g_processBgmEntries.end(),
                             [&](const AutoHookEntry &entry) { return matchAutoHookEntry(entry, exePath, exeName); });
    g_processBgmEntries.erase(it, g_processBgmEntries.end());
    if (enabled) {
        g_processBgmEntries.push_back(AutoHookEntry{exeName, exePath});
    }
    return saveAutoHookConfig(error);
}

std::size_t processBgmEntryCount() {
    std::lock_guard<std::mutex> lock(g_autoHookMutex);
    return g_processBgmEntries.size();
}

} // namespace krkrspeed::controller
