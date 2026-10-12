#include "ui_internal.h"

#include "../common/Logging.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <thread>

namespace krkrspeed::ui::detail {
namespace {

void logBackendForPid(HWND statusLabel, DWORD pid) {
    if (!statusLabel || pid == 0) return;
    const AudioBackend backend = controller::getAudioBackendForPid(pid);
    if (backend == AudioBackend::Unknown) return;

    auto it = g_loggedBackends.find(pid);
    if (it != g_loggedBackends.end() && it->second == backend) {
        return;
    }
    g_loggedBackends[pid] = backend;

    std::wstring backendName;
    switch (backend) {
    case AudioBackend::DirectSound:
        backendName = L"DirectSound";
        break;
    case AudioBackend::Wasapi:
        backendName = L"WASAPI";
        break;
    default:
        backendName = L"Other(" + std::to_wstring(static_cast<std::uint32_t>(backend)) + L")";
        break;
    }

    std::wstring label = L"PID " + std::to_wstring(pid);
    auto itName = g_hookedPidNames.find(pid);
    if (itName != g_hookedPidNames.end() && !itName->second.empty()) {
        label = itName->second + L" (PID " + std::to_wstring(pid) + L")";
    }
    setStatus(statusLabel, L"Audio backend for " + label + L": " + backendName);
}

// The PID is stored as item data so lookups never have to read (or parse) the label text.
DWORD comboItemPid(HWND combo, int index) {
    const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
    return data == CB_ERR ? 0 : static_cast<DWORD>(data);
}

void populateProcessCombo(HWND combo, const std::vector<ProcessInfo> &processes, DWORD prevPid) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int restoredIndex = -1;
    for (size_t i = 0; i < processes.size(); ++i) {
        const auto &proc = processes[i];
        std::wstring label;
        if (g_hookedPids.find(proc.pid) != g_hookedPids.end()) {
            label = L"[Injected] [" + std::to_wstring(proc.pid) + L"] " + proc.name;
        } else {
            label = L"[" + std::to_wstring(proc.pid) + L"] " + proc.name;
        }
        if (!proc.windowTitle.empty()) {
            label += L" | " + proc.windowTitle;
        }
        const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (index >= 0) {
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(proc.pid));
        }
        if (prevPid != 0 && proc.pid == prevPid) {
            restoredIndex = static_cast<int>(i);
        }
    }
    if (!processes.empty()) {
        SendMessageW(combo, CB_SETCURSEL, restoredIndex >= 0 ? restoredIndex : 0, 0);
    }
}

// Runs on a worker thread: no window access, the result is posted back to the UI thread.
InjectionResult performInjection(const ProcessInfo &proc, const controller::SharedConfig &cfg) {
    InjectionResult result;
    result.pid = proc.pid;
    result.name = proc.name;

    ProcessArch targetArch = ProcessArch::Unknown;
    std::wstring error;
    if (!controller::queryProcessArch(proc.pid, targetArch, error)) {
        result.message = error;
        return result;
    }
    if (!controller::writeSharedSettingsForPid(proc.pid, cfg, error)) {
        result.message = error;
        return result;
    }
    std::filesystem::path dllPath;
    if (!controller::selectHookForArch(controller::controllerDirectory(), targetArch, dllPath, error)) {
        result.message = error;
        return result;
    }
    ProcessArch dllArch = ProcessArch::Unknown;
    if (!controller::getDllArch(dllPath, dllArch, error)) {
        result.message = error;
        return result;
    }
    if (targetArch != ProcessArch::Unknown && dllArch != ProcessArch::Unknown && targetArch != dllArch) {
        result.message = L"Hook DLL arch (" + controller::describeArch(dllArch) + L") does not match target (" +
                         controller::describeArch(targetArch) + L"). Pick the correct dist folder.";
        return result;
    }
    if (!controller::injectDllIntoProcess(targetArch, proc.pid, dllPath, error)) {
        const ProcessArch selfArch = controller::getSelfArch();
        result.message = L"Injection failed: " + error + L" [controller=" + controller::describeArch(selfArch) +
                         L", target=" + controller::describeArch(targetArch) + L", dll=" +
                         controller::describeArch(dllArch) + L"]";
        return result;
    }
    result.ok = true;
    return result;
}

void postResult(HWND hwnd, UINT message, InjectionResult result) {
    auto owned = std::make_unique<InjectionResult>(std::move(result));
    if (hwnd && PostMessageW(hwnd, message, 0, reinterpret_cast<LPARAM>(owned.get()))) {
        owned.release(); // the window procedure takes ownership
    }
}

void postStatus(HWND hwnd, std::wstring text) {
    auto msg = std::make_unique<std::wstring>(std::move(text));
    if (hwnd && PostMessageW(hwnd, kMsgStatusUpdate, 0, reinterpret_cast<LPARAM>(msg.get()))) {
        msg.release();
    }
}

void scheduleAutoHook(HWND hwnd, const ProcessInfo &proc, controller::SharedConfig cfg, double delaySeconds) {
    std::thread([hwnd, proc, cfg, delaySeconds]() {
        if (delaySeconds > 0.0) {
            KRKR_LOG_INFO("Auto-hook delay " + std::to_string(delaySeconds) +
                          "s for pid=" + std::to_string(proc.pid));
            std::this_thread::sleep_for(std::chrono::duration<double>(delaySeconds));
        }
        const InjectionResult result = performInjection(proc, cfg);
        if (!result.ok) {
            KRKR_LOG_WARN(std::string("Auto-hook inject failed for pid=") + std::to_string(proc.pid));
            postStatus(hwnd, L"Auto-hook failed for " + proc.name + L" (PID " + std::to_wstring(proc.pid) + L"): " +
                                 result.message);
            return;
        }
        KRKR_LOG_INFO(std::string("Auto-hook injected pid=") + std::to_string(proc.pid));
        postStatus(hwnd, L"Auto-hook injected: " + proc.name + L" (PID " + std::to_wstring(proc.pid) + L")");
        if (hwnd) {
            PostMessageW(hwnd, kMsgAutoSelectPid, static_cast<WPARAM>(proc.pid), 0);
        }
    }).detach();
}

// Per-process config: the BGM flag is stored per game, everything else comes from the window state.
controller::SharedConfig buildSharedConfigForPid(DWORD pid, const std::wstring &name, float speed) {
    controller::SharedConfig cfg = buildSharedConfig(speed);
    std::wstring exePath;
    std::wstring error;
    if (!name.empty() && controller::getProcessExePath(pid, exePath, error)) {
        cfg.processAllAudio = controller::isProcessBgmEnabled(exePath, name);
    }
    return cfg;
}

} // namespace

std::wstring toLowerCopy(const std::wstring &input) {
    std::wstring out;
    out.reserve(input.size());
    for (wchar_t c : input) {
        out.push_back(static_cast<wchar_t>(std::towlower(c)));
    }
    return out;
}

void refreshProcessUi(HWND hwnd, HWND combo, HWND statusLabel) {
    if (!combo || !statusLabel) return;
    refreshProcessList(combo, statusLabel, true);
    updateAutoHookCheckbox(hwnd);
    updateProcessBgmCheckbox(hwnd);
    updateMinimizeToTrayControls(hwnd);
    const auto sessionProcesses = controller::enumerateSessionProcesses();
    std::unordered_set<DWORD> current;
    current.reserve(sessionProcesses.size());
    for (const auto &proc : sessionProcesses) {
        current.insert(proc.pid);
    }
    pruneHookedPids(current);
    updateHookButtonState(hwnd);
}

float readSpeedFromEdit(HWND edit) {
    wchar_t buffer[32] = {};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    wchar_t *end = nullptr;
    float parsed = std::wcstof(buffer, &end);
    if (end == buffer || !std::isfinite(parsed)) {
        // Restore previous valid value.
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", g_state.speed.lastValidSpeed);
        SetWindowTextW(edit, normalized);
        controller::updateSpeedFromInput(g_state.speed, g_state.speed.lastValidSpeed);
        return g_state.speed.lastValidSpeed;
    }
    if (parsed < 0.5f || parsed > 10.0f) {
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"%.2f", g_state.speed.lastValidSpeed);
        SetWindowTextW(edit, normalized);
        controller::updateSpeedFromInput(g_state.speed, g_state.speed.lastValidSpeed);
        return g_state.speed.lastValidSpeed;
    }

    controller::updateSpeedFromInput(g_state.speed, parsed);
    return g_state.speed.currentSpeed;
}

void writeSpeedEdit(HWND hwnd) {
    HWND edit = GetDlgItem(hwnd, kSpeedEditId);
    if (!edit) return;
    wchar_t normalized[32] = {};
    swprintf_s(normalized, L"%.2f", g_state.speed.currentSpeed);
    SetWindowTextW(edit, normalized);
}

void syncProcessAllAudioFromCheckbox(HWND hwnd) {
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    if (ignoreBgm) {
        g_state.processAllAudio = (SendMessageW(ignoreBgm, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
}

void updateAutoHookCheckbox(HWND hwnd) {
    if (!g_autoHookCheck) return;
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        SendMessageW(g_autoHookCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        updateAutoHookDelayControls(hwnd);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        SendMessageW(g_autoHookCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        updateAutoHookDelayControls(hwnd);
        return;
    }
    const bool enabled = controller::isAutoHookEnabled(exePath, proc.name);
    SendMessageW(g_autoHookCheck, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    updateAutoHookDelayControls(hwnd);
}

void updateProcessBgmCheckbox(HWND hwnd) {
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    if (!ignoreBgm) return;
    HWND speedEdit = GetDlgItem(hwnd, kSpeedEditId);
    bool tooltipChanged = false;
    auto applyTooltipMode = [&](bool wasapi) {
        const UiTextId bgmTip = wasapi ? UiTextId::TooltipProcessBgmWasapi : UiTextId::TooltipProcessBgm;
        const UiTextId speedTip = wasapi ? UiTextId::TooltipSpeedEditWasapi : UiTextId::TooltipSpeedEdit;
        tooltipChanged |= setTooltipId(ignoreBgm, bgmTip);
        tooltipChanged |= setTooltipId(g_ignoreBgmLabel, bgmTip);
        tooltipChanged |= setTooltipId(speedEdit, speedTip);
        tooltipChanged |= setTooltipId(g_speedLabel, speedTip);
    };
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        SendMessageW(ignoreBgm, BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(ignoreBgm, TRUE);
        g_state.processAllAudio = false;
        applyTooltipMode(false);
        if (tooltipChanged) {
            updateTooltips();
        }
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        SendMessageW(ignoreBgm, BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(ignoreBgm, TRUE);
        g_state.processAllAudio = false;
        applyTooltipMode(false);
        if (tooltipChanged) {
            updateTooltips();
        }
        return;
    }
    const bool wasapiActive = controller::isWasapiActiveForPid(proc.pid);
    if (wasapiActive) {
        SendMessageW(ignoreBgm, BM_SETCHECK, BST_CHECKED, 0);
        EnableWindow(ignoreBgm, FALSE);
        g_state.processAllAudio = true;
        applyTooltipMode(true);
        if (tooltipChanged) {
            updateTooltips();
        }
        return;
    }
    const bool enabled = controller::isProcessBgmEnabled(exePath, proc.name);
    SendMessageW(ignoreBgm, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(ignoreBgm, TRUE);
    g_state.processAllAudio = enabled;
    applyTooltipMode(false);
    if (tooltipChanged) {
        updateTooltips();
    }
}

void updateAutoHookDelayControls(HWND hwnd) {
    if (!g_autoHookDelayLabel || !g_autoHookDelayCheck) return;
    bool autoHookEnabled =
        g_autoHookCheck && (SendMessageW(g_autoHookCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ProcessInfo proc;
    std::wstring error;
    std::wstring exePath;
    if (autoHookEnabled) {
        if (!getSelectedProcess(hwnd, proc, error) ||
            !controller::getProcessExePath(proc.pid, exePath, error)) {
            autoHookEnabled = false;
        }
    }

    ShowWindow(g_autoHookDelayLabel, SW_SHOW);
    ShowWindow(g_autoHookDelayCheck, SW_SHOW);
    EnableWindow(g_autoHookDelayLabel, autoHookEnabled);
    EnableWindow(g_autoHookDelayCheck, autoHookEnabled);
    if (!autoHookEnabled) {
        SendMessageW(g_autoHookDelayCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        return;
    }

    double delaySeconds = 0.0;
    const bool hasDelay = controller::tryGetAutoHookDelay(exePath, proc.name, delaySeconds);
    SendMessageW(g_autoHookDelayCheck, BM_SETCHECK, hasDelay ? BST_CHECKED : BST_UNCHECKED, 0);
}

void updateMinimizeToTrayControls(HWND hwnd) {
    if (!g_minimizeToTrayCheck) return;
    const bool enabled = controller::getMinimizeToTrayOnClose();
    g_state.minimizeToTrayOnClose = enabled;
    SendMessageW(g_minimizeToTrayCheck, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(g_minimizeToTrayCheck, TRUE);
    if (g_minimizeToTrayLabel) {
        EnableWindow(g_minimizeToTrayLabel, TRUE);
    }
}

void updateHookButtonState(HWND hwnd) {
    HWND hookButton = GetDlgItem(hwnd, kRefreshButtonId);
    if (!hookButton) return;

    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        SetWindowTextW(hookButton, L"Hook");
        EnableWindow(hookButton, TRUE);
        return;
    }

    const bool hooked = (g_hookedPids.find(proc.pid) != g_hookedPids.end());
    if (g_injectionInProgress) {
        EnableWindow(hookButton, FALSE);
    } else if (hooked) {
        SetWindowTextW(hookButton, ui_text::UiText(UiTextId::ButtonHooked).c_str());
        EnableWindow(hookButton, FALSE);
    } else {
        SetWindowTextW(hookButton, ui_text::UiText(UiTextId::ButtonHook).c_str());
        EnableWindow(hookButton, TRUE);
    }
}

void applySettingsToSelectedIfHooked(HWND hwnd) {
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        return;
    }
    if (g_hookedPids.find(proc.pid) == g_hookedPids.end()) {
        return;
    }
    controller::SharedConfig cfg = buildSharedConfig(g_state.speed.currentSpeed);
    if (!controller::applySpeedToPid(proc.pid, cfg, g_state.speed, error)) {
        setStatus(GetDlgItem(hwnd, kStatusLabelId), error);
    }
}

bool selectProcessByPid(HWND hwnd, DWORD pid) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    if (!combo || pid == 0) return false;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        if (comboItemPid(combo, i) == pid) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            updateAutoHookCheckbox(hwnd);
            updateProcessBgmCheckbox(hwnd);
            updateHookButtonState(hwnd);
            return true;
        }
    }
    return false;
}

void initKnownPids() {
    g_knownPids.clear();
    g_autoHookAttempted.clear();
    const auto sessionProcesses = controller::enumerateSessionProcesses();
    for (const auto &proc : sessionProcesses) {
        g_knownPids.insert(proc.pid);
    }
}

bool pruneHookedPids(const std::unordered_set<DWORD> &current, std::vector<RemovedHookInfo> *removed) {
    bool removedAny = false;
    for (auto it = g_hookedPids.begin(); it != g_hookedPids.end();) {
        if (current.find(*it) == current.end()) {
            if (removed) {
                RemovedHookInfo info{};
                info.pid = *it;
                auto itName = g_hookedPidNames.find(*it);
                if (itName != g_hookedPidNames.end()) {
                    info.name = itName->second;
                }
                removed->push_back(std::move(info));
            }
            g_hookedPidNames.erase(*it);
            g_loggedBackends.erase(*it);
            it = g_hookedPids.erase(it);
            removedAny = true;
        } else {
            ++it;
        }
    }
    return removedAny;
}

void scheduleAutoHooksForProcesses(HWND hwnd, const std::vector<ProcessInfo> &processes, bool updateUi) {
    if (processes.empty() || controller::autoHookEntryCount() == 0) {
        return;
    }

    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    if (editSpeed) {
        readSpeedFromEdit(editSpeed);
    }
    syncProcessAllAudioFromCheckbox(hwnd);
    const float effectiveSpeed = controller::effectiveSpeed(g_state.speed);
    controller::SharedConfig cfg = buildSharedConfig(effectiveSpeed);

    std::unordered_set<std::wstring> autoHookNames;
    {
        const auto names = controller::autoHookExeNames();
        autoHookNames.reserve(names.size());
        for (const auto &name : names) {
            autoHookNames.insert(toLowerCopy(name));
        }
    }

    HWND combo = nullptr;
    HWND statusLabel = nullptr;
    bool refreshedUi = false;
    if (updateUi) {
        combo = GetDlgItem(hwnd, kProcessComboId);
        statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    }

    for (const auto &proc : processes) {
        if (g_autoHookAttempted.find(proc.pid) != g_autoHookAttempted.end()) continue;
        if (!autoHookNames.empty()) {
            const std::wstring nameLower = toLowerCopy(proc.name);
            if (autoHookNames.find(nameLower) == autoHookNames.end()) {
                continue;
            }
        }
        std::wstring exePath;
        std::wstring error;
        if (!controller::getProcessExePath(proc.pid, exePath, error)) {
            continue;
        }
        if (!controller::isAutoHookEnabled(exePath, proc.name)) {
            continue;
        }
        controller::SharedConfig procCfg = cfg;
        procCfg.processAllAudio = controller::isProcessBgmEnabled(exePath, proc.name);
        double delaySeconds = 0.0;
        controller::tryGetAutoHookDelay(exePath, proc.name, delaySeconds);
        g_autoHookAttempted.insert(proc.pid);

        if (updateUi && combo && statusLabel) {
            if (!refreshedUi) {
                refreshProcessUi(hwnd, combo, statusLabel);
                refreshedUi = true;
            }
            if (!selectProcessByPid(hwnd, proc.pid)) {
                g_pendingAutoSelectPid = proc.pid;
                g_pendingAutoHookRefresh = true;
            }
        }

        scheduleAutoHook(hwnd, proc, procCfg, delaySeconds);
    }
}

void pollAutoHook(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    if (!combo || !statusLabel) return;

    if (g_pendingAutoHookRefresh) {
        refreshProcessUi(hwnd, combo, statusLabel);
        bool selected = false;
        if (g_pendingAutoSelectPid != 0) {
            selected = selectProcessByPid(hwnd, g_pendingAutoSelectPid);
            if (selected) {
                g_pendingAutoSelectPid = 0;
            }
        }
        if (selected || g_pendingAutoSelectPid == 0) {
            g_pendingAutoHookRefresh = false;
        }
    } else if (g_pendingAutoSelectPid != 0) {
        if (selectProcessByPid(hwnd, g_pendingAutoSelectPid)) {
            g_pendingAutoSelectPid = 0;
        }
    }

    if (controller::autoHookEntryCount() == 0 && g_hookedPids.empty()) {
        return;
    }

    std::unordered_set<DWORD> current;
    std::vector<ProcessInfo> newProcesses;
    const auto sessionProcesses = controller::enumerateSessionProcesses();
    current.reserve(sessionProcesses.size());
    for (const auto &proc : sessionProcesses) {
        current.insert(proc.pid);
        if (g_knownPids.find(proc.pid) == g_knownPids.end()) {
            newProcesses.push_back(proc);
        }
    }

    bool removedInjected = false;
    for (auto it = g_autoHookAttempted.begin(); it != g_autoHookAttempted.end();) {
        if (current.find(*it) == current.end()) {
            it = g_autoHookAttempted.erase(it);
            removedInjected = true;
        } else {
            ++it;
        }
    }

    std::vector<RemovedHookInfo> removedHooks;
    const bool removedHooked = pruneHookedPids(current, &removedHooks);
    bool refreshed = false;
    if (removedHooked) {
        refreshProcessUi(hwnd, combo, statusLabel);
        refreshed = true;
        for (const auto &info : removedHooks) {
            std::wstring label = info.name.empty()
                ? (L"PID " + std::to_wstring(info.pid))
                : (info.name + L" (PID " + std::to_wstring(info.pid) + L")");
            setStatus(statusLabel, L"Hooked process exited: " + label);
        }
    }
    if (removedInjected && !refreshed) {
        refreshProcessUi(hwnd, combo, statusLabel);
    }

    if (g_pendingAutoSelectPid != 0 && current.find(g_pendingAutoSelectPid) == current.end()) {
        g_pendingAutoSelectPid = 0;
        g_pendingAutoHookRefresh = false;
    }

    g_knownPids = std::move(current);

    for (const auto pid : g_hookedPids) {
        logBackendForPid(statusLabel, pid);
    }

    if (newProcesses.empty()) return;
    scheduleAutoHooksForProcesses(hwnd, newProcesses, true);
}

void handleAutoHookToggle(HWND hwnd) {
    if (!g_autoHookCheck) return;
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        setStatus(statusLabel, error);
        return;
    }
    const bool checked = (SendMessageW(g_autoHookCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!controller::setAutoHookEnabled(exePath, proc.name, checked, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring msg = checked ? (L"Auto-hook enabled for " + proc.name)
                               : (L"Auto-hook disabled for " + proc.name);
    setStatus(statusLabel, msg);
    if (controller::autoHookEntryCount() > 0) {
        initKnownPids();
    }
    updateAutoHookDelayControls(hwnd);
}

void handleAutoHookDelayToggle(HWND hwnd) {
    if (!g_autoHookDelayCheck) return;
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        setStatus(statusLabel, error);
        return;
    }
    const bool checked = (SendMessageW(g_autoHookDelayCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const double delaySeconds = checked ? 15.0 : 0.0;
    if (!controller::setAutoHookDelay(exePath, proc.name, checked, delaySeconds, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring msg = checked ? (L"Auto-hook delay enabled for " + proc.name)
                               : (L"Auto-hook delay disabled for " + proc.name);
    setStatus(statusLabel, msg);
}

void handleMinimizeToTrayToggle(HWND hwnd) {
    if (!g_minimizeToTrayCheck) return;
    const bool checked = (SendMessageW(g_minimizeToTrayCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    std::wstring error;
    if (!controller::setMinimizeToTrayOnClose(checked, error)) {
        HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
        setStatus(statusLabel, error);
        const bool current = controller::getMinimizeToTrayOnClose();
        SendMessageW(g_minimizeToTrayCheck, BM_SETCHECK, current ? BST_CHECKED : BST_UNCHECKED, 0);
        g_state.minimizeToTrayOnClose = current;
        return;
    }
    g_state.minimizeToTrayOnClose = checked;
    updateMinimizeToTrayControls(hwnd);
}

void handleProcessBgmToggle(HWND hwnd) {
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    if (!ignoreBgm) return;
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring exePath;
    if (!controller::getProcessExePath(proc.pid, exePath, error)) {
        setStatus(statusLabel, error);
        return;
    }
    const bool checked = (SendMessageW(ignoreBgm, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!controller::setProcessBgmEnabled(exePath, proc.name, checked, error)) {
        setStatus(statusLabel, error);
        return;
    }
    std::wstring msg = checked ? (L"Process BGM enabled for " + proc.name)
                               : (L"Process BGM disabled for " + proc.name);
    setStatus(statusLabel, msg);
}

controller::SharedConfig buildSharedConfig(float speed) {
    controller::SharedConfig cfg{};
    cfg.speed = speed;
    cfg.lengthGateEnabled = true;
    cfg.enableLog = g_state.enableLog;
    cfg.processAllAudio = g_state.processAllAudio;
    cfg.stereoBgmMode = g_state.stereoBgmMode;
    cfg.bgmSeconds = g_state.bgmSeconds;
    return cfg;
}

bool getSelectedProcess(HWND hwnd, ProcessInfo &proc, std::wstring &error) {
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_state.processes.size())) {
        error = L"Select a process first.";
        return false;
    }
    proc = g_state.processes[static_cast<std::size_t>(index)];
    return true;
}

void refreshProcessList(HWND combo, HWND statusLabel, bool quiet) {
    const int prevSel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    const DWORD prevPid = prevSel >= 0 ? comboItemPid(combo, prevSel) : 0;
    g_state.processes = controller::enumerateVisibleProcesses();
    populateProcessCombo(combo, g_state.processes, prevPid);
    if (!quiet) {
        std::wstring status = L"Found " + std::to_wstring(g_state.processes.size()) + L" processes";
        setStatus(statusLabel, status);
    }
}

void applySpeedToHookedProcesses(HWND hwnd) {
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    const float speed = controller::effectiveSpeed(g_state.speed);
    for (const DWORD pid : g_hookedPids) {
        auto itName = g_hookedPidNames.find(pid);
        const std::wstring name = itName != g_hookedPidNames.end() ? itName->second : std::wstring{};
        std::wstring error;
        controller::SharedConfig cfg = buildSharedConfigForPid(pid, name, speed);
        if (!controller::writeSharedSettingsForPid(pid, cfg, error)) {
            setStatus(statusLabel, error);
        }
    }
}

void handleSpeedHotkey(HWND hwnd, SpeedHotkeyAction action) {
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    if (editSpeed) {
        readSpeedFromEdit(editSpeed);
    }
    syncProcessAllAudioFromCheckbox(hwnd);
    const std::wstring status = controller::applySpeedHotkey(g_state.speed, action);
    writeSpeedEdit(hwnd);
    if (!g_hookedPids.empty()) {
        // Hotkeys are pressed in-game, so they target every hooked game rather than the dropdown selection.
        applySpeedToHookedProcesses(hwnd);
    } else {
        ProcessInfo proc;
        std::wstring error;
        if (getSelectedProcess(hwnd, proc, error)) {
            if (!controller::applySpeedToPid(proc.pid, buildSharedConfig(g_state.speed.currentSpeed), g_state.speed,
                                             error)) {
                setStatus(statusLabel, error);
                return;
            }
        }
    }
    setStatus(statusLabel, status);
}

void handleApply(HWND hwnd) {
    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    HWND ignoreBgm = GetDlgItem(hwnd, kIgnoreBgmCheckId);
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);

    if (g_injectionInProgress) {
        setStatus(statusLabel, L"Injection already in progress...");
        return;
    }
    readSpeedFromEdit(editSpeed);
    g_state.processAllAudio = (ignoreBgm && SendMessageW(ignoreBgm, BM_GETCHECK, 0, 0) == BST_CHECKED);

    ProcessInfo proc;
    std::wstring error;
    if (!getSelectedProcess(hwnd, proc, error)) {
        setStatus(statusLabel, error);
        return;
    }

    controller::SharedConfig cfg = buildSharedConfig(controller::effectiveSpeed(g_state.speed));
    g_injectionInProgress = true;
    updateHookButtonState(hwnd);
    setStatus(statusLabel, L"Injecting into " + proc.name + L" (PID " + std::to_wstring(proc.pid) + L")...");
    // Injection can wait several seconds (and show a UAC prompt); keep the window responsive.
    std::thread([hwnd, proc, cfg]() {
        postResult(hwnd, kMsgInjectionDone, performInjection(proc, cfg));
    }).detach();
}

void handleInjectionDone(HWND hwnd, const InjectionResult &result) {
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    g_injectionInProgress = false;
    if (!result.ok) {
        setStatus(statusLabel, result.message);
        updateHookButtonState(hwnd);
        return;
    }
    g_hookedPids.insert(result.pid);
    g_hookedPidNames[result.pid] = result.name;
    updateHookButtonState(hwnd);
    updateProcessBgmCheckbox(hwnd);
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    if (combo) {
        refreshProcessUi(hwnd, combo, statusLabel);
    }
    wchar_t message[160] = {};
    const float effectiveSpeed = controller::effectiveSpeed(g_state.speed);
    if (g_state.speed.enabled) {
        swprintf_s(message, L"Injected into %s (PID %u) at %.2fx; gate on @ %.2fs",
                   result.name.c_str(), result.pid, effectiveSpeed, g_state.bgmSeconds);
    } else {
        swprintf_s(message, L"Injected into %s (PID %u) at %.2fx (speed off); gate on @ %.2fs",
                   result.name.c_str(), result.pid, effectiveSpeed, g_state.bgmSeconds);
    }
    setStatus(statusLabel, message);
}

void handleLaunch(HWND hwnd) {
    HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
    HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
    if (editSpeed) {
        readSpeedFromEdit(editSpeed);
    }
    if (g_state.launchPath.empty()) {
        setStatus(statusLabel, L"Launch path not set.");
        return;
    }
    const std::filesystem::path exePath = g_state.launchPath;
    const controller::SharedConfig cfg = buildSharedConfig(controller::effectiveSpeed(g_state.speed));
    setStatus(statusLabel, L"Launching " + exePath.filename().wstring() + L"...");
    std::thread([hwnd, exePath, cfg]() {
        InjectionResult result;
        result.name = exePath.filename().wstring();
        DWORD launchedPid = 0;
        std::wstring err;
        result.ok = controller::launchAndInject(exePath, cfg, launchedPid, err);
        result.pid = launchedPid;
        result.message = result.ok ? L"Launched and injected: " + result.name + L" (PID " +
                                         std::to_wstring(launchedPid) + L")"
                                   : err;
        postResult(hwnd, kMsgLaunchDone, std::move(result));
    }).detach();
}

void handleLaunchDone(HWND hwnd, const InjectionResult &result) {
    setStatus(GetDlgItem(hwnd, kStatusLabelId), result.message);
    if (!result.ok) {
        return;
    }
    g_hookedPids.insert(result.pid);
    if (!result.name.empty()) {
        g_hookedPidNames[result.pid] = result.name;
    }
    // The game needs a moment to open its window; poll the list briefly and select it once visible.
    g_launchRefreshPid = result.pid;
    g_launchRefreshTicks = 0;
    SetTimer(hwnd, kLaunchRefreshTimerId, kLaunchRefreshIntervalMs, nullptr);
}

void handleLaunchRefreshTick(HWND hwnd) {
    refreshProcessList(GetDlgItem(hwnd, kProcessComboId), GetDlgItem(hwnd, kStatusLabelId), true);
    updateAutoHookCheckbox(hwnd);
    updateProcessBgmCheckbox(hwnd);
    updateMinimizeToTrayControls(hwnd);
    updateHookButtonState(hwnd);
    if (selectProcessByPid(hwnd, g_launchRefreshPid) || ++g_launchRefreshTicks >= kLaunchRefreshMaxTicks) {
        KillTimer(hwnd, kLaunchRefreshTimerId);
        g_launchRefreshPid = 0;
    }
}

void handleAutoSelectPid(HWND hwnd, DWORD pid) {
    g_pendingAutoSelectPid = pid;
    g_pendingAutoHookRefresh = true;
    g_hookedPids.insert(pid);
    if (g_hookedPidNames.find(pid) == g_hookedPidNames.end()) {
        const auto session = controller::enumerateSessionProcesses();
        for (const auto &proc : session) {
            if (proc.pid == pid) {
                g_hookedPidNames[pid] = proc.name;
                break;
            }
        }
    }
    HWND combo = GetDlgItem(hwnd, kProcessComboId);
    if (combo) {
        refreshProcessUi(hwnd, combo, GetDlgItem(hwnd, kStatusLabelId));
        if (selectProcessByPid(hwnd, pid)) {
            g_pendingAutoSelectPid = 0;
            g_pendingAutoHookRefresh = false;
        }
    }
}

} // namespace krkrspeed::ui::detail
