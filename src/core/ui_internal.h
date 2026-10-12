#pragma once

// Shared state and helpers for the controller window, split across ui.cpp (window procedure),
// ui_actions.cpp (process list, injection, auto-hook) and ui_widgets.cpp (status, tooltips, tray, layout).

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "ui.h"
#include "ControllerCore.h"

#include "../common/SharedStatus.h"
#include "../common/UiText.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace krkrspeed::ui::detail {

using controller::ProcessArch;
using controller::ProcessInfo;
using controller::SpeedControlState;
using controller::SpeedHotkeyAction;
using ui_text::UiTextId;

constexpr int kProcessComboId = 1001;
constexpr int kRefreshButtonId = 1002;
constexpr int kSpeedEditId = 1003;
constexpr int kStatusLabelId = 1005;
constexpr int kLinkId = 1006;
constexpr int kIgnoreBgmCheckId = 1009;
constexpr int kIgnoreBgmLabelId = 1010;
constexpr int kAutoHookCheckId = 1011;
constexpr int kAutoHookLabelId = 1012;
constexpr int kLanguageComboId = 1013;
constexpr int kAutoHookDelayCheckId = 1014;
constexpr int kAutoHookDelayLabelId = 1015;
constexpr int kMinimizeToTrayCheckId = 1016;
constexpr int kMinimizeToTrayLabelId = 1017;
constexpr UINT kAutoHookTimerId = 3001;
constexpr UINT kAutoHookIntervalMs = 3000;
constexpr UINT kLaunchRefreshTimerId = 3002;
constexpr UINT kLaunchRefreshIntervalMs = 500;
constexpr int kLaunchRefreshMaxTicks = 6;
constexpr UINT kMsgAutoSelectPid = WM_APP + 2;
constexpr UINT kMsgStatusUpdate = WM_APP + 3;
constexpr UINT kTrayCallbackMsg = WM_APP + 4;
constexpr UINT kMsgInjectionDone = WM_APP + 5;
constexpr UINT kMsgLaunchDone = WM_APP + 6;
constexpr std::size_t kStatusMaxChars = 128 * 1024 / sizeof(wchar_t);
constexpr int kHotkeyToggleSpeedId = 2001;
constexpr int kHotkeySpeedUpId = 2002;
constexpr int kHotkeySpeedDownId = 2003;
constexpr UINT kHotkeyModifiers = MOD_ALT;
constexpr UINT kTrayMenuExitId = 4001;
inline const std::wstring kAppVersion = L"1.2.1";

struct AppState {
    std::vector<ProcessInfo> processes;
    SpeedControlState speed;
    std::filesystem::path launchPath;
    bool enableLog = false;
    bool processAllAudio = false;
    bool minimizeToTrayOnClose = false;
    float bgmSeconds = 60.0f; // also used as length gate seconds
    std::uint32_t stereoBgmMode = 1;
    std::wstring searchTerm;
};

struct RemovedHookInfo {
    DWORD pid = 0;
    std::wstring name;
};

// Result of a background injection or launch, posted back to the window (lParam owns it).
struct InjectionResult {
    DWORD pid = 0;
    std::wstring name;
    bool ok = false;
    std::wstring message;
};

// All window state lives on the UI thread; worker threads only post messages.
inline AppState g_state;
inline HWND g_link = nullptr;
inline bool g_linkIsSysLink = false;
inline ControllerOptions g_initialOptions{};
inline HWND g_processLabel = nullptr;
inline HWND g_speedLabel = nullptr;
inline HWND g_autoHookCheck = nullptr;
inline HWND g_autoHookLabel = nullptr;
inline HWND g_autoHookDelayCheck = nullptr;
inline HWND g_autoHookDelayLabel = nullptr;
inline HWND g_ignoreBgmLabel = nullptr;
inline HWND g_languageCombo = nullptr;
inline HWND g_hotkeyLabel = nullptr;
inline HWND g_infoLabel = nullptr;
inline HWND g_minimizeToTrayCheck = nullptr;
inline HWND g_minimizeToTrayLabel = nullptr;
inline HWND g_tooltip = nullptr;
inline std::unordered_map<std::uintptr_t, UiTextId> g_tooltipById;
inline HWND g_mainWindow = nullptr;
inline HWND g_activeTooltipControl = nullptr;
inline HFONT g_hotkeyFont = nullptr;
inline std::unordered_set<DWORD> g_knownPids;
inline std::unordered_set<DWORD> g_autoHookAttempted;
inline std::unordered_set<DWORD> g_hookedPids;
inline std::unordered_map<DWORD, std::wstring> g_hookedPidNames;
inline std::unordered_map<DWORD, AudioBackend> g_loggedBackends;
inline DWORD g_pendingAutoSelectPid = 0;
inline bool g_pendingAutoHookRefresh = false;
inline bool g_injectionInProgress = false;
inline DWORD g_launchRefreshPid = 0;
inline int g_launchRefreshTicks = 0;
inline bool g_trayIconVisible = false;
inline bool g_allowClose = false;
inline NOTIFYICONDATAW g_trayIcon{};
inline WNDPROC g_speedEditProc = nullptr;
inline std::wstring g_statusBuffer;

std::wstring toLowerCopy(const std::wstring &input);

// ui_widgets.cpp
void setStatus(HWND statusLabel, const std::wstring &text);
void scrollStatusToBottom(HWND statusLabel);
void toggleCheckboxFromLabel(HWND hwnd, int checkId);
void ensureUiTextLoaded();
void refreshUiText(HWND hwnd);
void layoutControls(HWND hwnd);
int getTextHeight(HWND hwnd, int fallback);
HFONT createHotkeyFont(HWND reference);
HWND createTooltip(HWND parent);
void addTooltip(HWND tooltip, HWND control, UiTextId textId);
bool setTooltipId(HWND control, UiTextId textId);
void updateTooltips();
void updateTrackedTooltip(const MSG &msg);
void showTrayIcon(HWND hwnd);
void hideTrayIcon();
void restoreFromTray(HWND hwnd);
void showTrayMenu(HWND hwnd);
void registerControllerHotkeys(HWND hwnd, HWND statusLabel);

// ui_actions.cpp
bool getSelectedProcess(HWND hwnd, ProcessInfo &proc, std::wstring &error);
void refreshProcessList(HWND combo, HWND statusLabel, bool quiet = false);
void refreshProcessUi(HWND hwnd, HWND combo, HWND statusLabel);
bool selectProcessByPid(HWND hwnd, DWORD pid);
controller::SharedConfig buildSharedConfig(float speed);
float readSpeedFromEdit(HWND edit);
void writeSpeedEdit(HWND hwnd);
void syncProcessAllAudioFromCheckbox(HWND hwnd);
void updateAutoHookCheckbox(HWND hwnd);
void updateProcessBgmCheckbox(HWND hwnd);
void updateAutoHookDelayControls(HWND hwnd);
void updateMinimizeToTrayControls(HWND hwnd);
void updateHookButtonState(HWND hwnd);
void applySettingsToSelectedIfHooked(HWND hwnd);
void applySpeedToHookedProcesses(HWND hwnd);
void handleSpeedHotkey(HWND hwnd, SpeedHotkeyAction action);
bool pruneHookedPids(const std::unordered_set<DWORD> &current, std::vector<RemovedHookInfo> *removed = nullptr);
void initKnownPids();
void scheduleAutoHooksForProcesses(HWND hwnd, const std::vector<ProcessInfo> &processes, bool updateUi);
void pollAutoHook(HWND hwnd);
void handleAutoHookToggle(HWND hwnd);
void handleAutoHookDelayToggle(HWND hwnd);
void handleMinimizeToTrayToggle(HWND hwnd);
void handleProcessBgmToggle(HWND hwnd);
void handleApply(HWND hwnd);
void handleInjectionDone(HWND hwnd, const InjectionResult &result);
void handleLaunch(HWND hwnd);
void handleLaunchDone(HWND hwnd, const InjectionResult &result);
void handleLaunchRefreshTick(HWND hwnd);
void handleAutoSelectPid(HWND hwnd, DWORD pid);

} // namespace krkrspeed::ui::detail
