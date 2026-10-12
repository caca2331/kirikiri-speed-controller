#pragma once

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include <Windows.h>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "../common/SharedStatus.h"

namespace krkrspeed::controller {

enum class ProcessArch { Unknown, X86, X64, Arm64 };

struct ProcessInfo {
    std::wstring name;
    std::wstring windowTitle;
    DWORD pid = 0;
    ProcessArch arch = ProcessArch::Unknown;
    bool hasWindow = false;
};

struct SharedConfig {
    float speed = 1.5f;
    bool lengthGateEnabled = true;
    float bgmSeconds = 60.0f;
    bool enableLog = false;
    bool processAllAudio = false;
    std::uint32_t stereoBgmMode = 1;
};

struct AutoHookEntry {
    std::wstring exeName;
    std::wstring exePath;
    bool hasDelay = false;
    double delaySeconds = 0.0;
};

struct SpeedControlState {
    float currentSpeed = 1.5f;
    float lastValidSpeed = 1.5f;
    bool enabled = true;
};

enum class SpeedHotkeyAction {
    Toggle,
    SpeedUp,
    SpeedDown
};

ProcessArch getSelfArch();
bool ensureDebugPrivilege();
std::filesystem::path controllerDirectory();
std::vector<ProcessInfo> enumerateVisibleProcesses();
std::vector<ProcessInfo> enumerateSessionProcesses();
bool queryProcessArch(DWORD pid, ProcessArch &archOut, std::wstring &error);
bool getProcessExePath(DWORD pid, std::wstring &pathOut, std::wstring &error);
bool getDllArch(const std::filesystem::path &path, ProcessArch &archOut, std::wstring &error);
bool selectHookForArch(const std::filesystem::path &controllerDir, ProcessArch targetArch, std::filesystem::path &outPath,
                       std::wstring &error);
bool writeSharedSettingsForPid(DWORD pid, const SharedConfig &config, std::wstring &error);
AudioBackend getAudioBackendForPid(DWORD pid);
bool isWasapiActiveForPid(DWORD pid);
bool injectDllIntoProcess(ProcessArch targetArch, DWORD pid, const std::filesystem::path &dllPath, std::wstring &error);
bool launchAndInject(const std::filesystem::path &exePath, const SharedConfig &config, DWORD &outPid, std::wstring &error);
std::wstring describeArch(ProcessArch arch);

void loadAutoHookConfig();
bool isAutoHookEnabled(const std::wstring &exePath, const std::wstring &exeName);
bool setAutoHookEnabled(const std::wstring &exePath, const std::wstring &exeName, bool enabled, std::wstring &error);
std::size_t autoHookEntryCount();
std::vector<std::wstring> autoHookExeNames();
bool tryGetAutoHookDelay(const std::wstring &exePath, const std::wstring &exeName, double &outDelaySeconds);
bool setAutoHookDelay(const std::wstring &exePath, const std::wstring &exeName, bool enabled, double delaySeconds,
                      std::wstring &error);
bool getMinimizeToTrayOnClose();
bool setMinimizeToTrayOnClose(bool enabled, std::wstring &error);
// UI language code saved in krkr_speed_config.yaml ("en", "zh-CN", "ja"); empty if never chosen.
std::wstring getUiLanguage();
bool setUiLanguage(const std::wstring &langCode, std::wstring &error);
bool isProcessBgmEnabled(const std::wstring &exePath, const std::wstring &exeName);
bool setProcessBgmEnabled(const std::wstring &exePath, const std::wstring &exeName, bool enabled, std::wstring &error);
std::size_t processBgmEntryCount();

float clampSpeed(float speed);
float roundSpeed(float speed);
float effectiveSpeed(const SpeedControlState &state);
void initSpeedState(SpeedControlState &state, float speed, bool enabled = true);
void updateSpeedFromInput(SpeedControlState &state, float speed);
bool applySpeedToPid(DWORD pid, const SharedConfig &baseConfig, const SpeedControlState &state, std::wstring &error);
// Updates the speed state for a hotkey and returns the status line; callers push the speed to processes.
std::wstring applySpeedHotkey(SpeedControlState &state, SpeedHotkeyAction action);

} // namespace krkrspeed::controller
