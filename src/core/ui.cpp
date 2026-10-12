#include "ui_internal.h"
#include "resource.h"

#include "../common/Logging.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>

namespace krkrspeed::ui {
namespace {
using namespace detail;

const UINT kMsgRestoreWindow = RegisterWindowMessageW(L"KrkrSpeedController_Restore");

LRESULT CALLBACK SpeedEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            wchar_t normalized[32] = {};
            swprintf_s(normalized, L"%.2f", g_state.speed.lastValidSpeed);
            SetWindowTextW(hwnd, normalized);
            SetFocus(GetParent(hwnd));
            return 0;
        }
        if (wParam == VK_RETURN) {
            SetFocus(GetParent(hwnd));
            return 0;
        }
    } else if (msg == WM_CHAR) {
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
            return 0;
        }
    }
    return CallWindowProcW(g_speedEditProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kMsgRestoreWindow) {
        restoreFromTray(hwnd);
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        g_mainWindow = hwnd;
        ensureUiTextLoaded();
        RECT rcClient{};
        GetClientRect(hwnd, &rcClient);
        int initialWidth = rcClient.right - 120 - 120 - 12 * 3;
        HWND combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                     132, 10, initialWidth, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessComboId)), nullptr, nullptr);
        HWND refresh = CreateWindowExW(0, L"BUTTON", ui_text::UiText(UiTextId::ButtonHook).c_str(),
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 10, 100, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)), nullptr, nullptr);

        g_processLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelProcess).c_str(),
                                         WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_RIGHT, 12, 12, 100, 20, hwnd, nullptr, nullptr, nullptr);
        g_speedLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelSpeed).c_str(),
                                       WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_RIGHT, 12, 68, 100, 20, hwnd, nullptr, nullptr, nullptr);
        wchar_t speedText[32] = {};
        swprintf_s(speedText, L"%.2f", g_state.speed.currentSpeed);
        HWND speedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", speedText,
                                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         140, 66, 40, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeedEditId)), nullptr, nullptr);
        if (speedEdit && !g_speedEditProc) {
            g_speedEditProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(speedEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SpeedEditProc)));
        }
        HWND ignoreBgm = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         140 + 40 + 12 + 90, 66, 20, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIgnoreBgmCheckId)), nullptr, nullptr);
        g_ignoreBgmLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelProcessBgm).c_str(),
                                           WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_RIGHT,
                                           140 + 40 + 12, 66, 90, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIgnoreBgmLabelId)), nullptr, nullptr);
        g_autoHookLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelAutoHook).c_str(),
                                          WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_RIGHT,
                                          140 + 40 + 12 + 90 + 20 + 12, 66, 135, 20,
                                          hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoHookLabelId)), nullptr, nullptr);
        g_autoHookCheck = CreateWindowExW(0, L"BUTTON", L"",
                                          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                          140 + 40 + 12 + 90 + 20 + 12 + 135 + 6, 66, 20, 20,
                                          hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoHookCheckId)), nullptr, nullptr);
        g_autoHookDelayLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelAutoHookDelay).c_str(),
                                               WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_RIGHT,
                                               140 + 40 + 12 + 90 + 20 + 12 + 135 + 6 + 20 + 12, 66, 140, 20,
                                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoHookDelayLabelId)), nullptr, nullptr);
        g_autoHookDelayCheck = CreateWindowExW(0, L"BUTTON", L"",
                                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                               140 + 40 + 12 + 90 + 20 + 12 + 135 + 6 + 20 + 12 + 140 + 6, 66, 20, 20,
                                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoHookDelayCheckId)), nullptr, nullptr);
        g_languageCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                          0, 66, 120, 200,
                                          hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLanguageComboId)), nullptr, nullptr);
        if (g_languageCombo) {
            SendMessageW(g_languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
            SendMessageW(g_languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"中文 Chinese"));
            SendMessageW(g_languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"日本語 Japanese"));
            SendMessageW(g_languageCombo, CB_SETCURSEL, 1, 0);
        }
        if (ignoreBgm) {
            SendMessageW(ignoreBgm, BM_SETCHECK, g_state.processAllAudio ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        // Hook + Apply button removed; "Hook" button handles injection.

        HWND statusEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                                              ES_READONLY | WS_VSCROLL,
                                          12, 96, 400, 20, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabelId)), nullptr, nullptr);
        if (statusEdit) {
            SendMessageW(statusEdit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            setStatus(statusEdit, L"Ready");
        }

        g_minimizeToTrayLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelMinimizeToTray).c_str(),
                                                WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_RIGHT,
                                                12, 0, 220, 20, hwnd,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMinimizeToTrayLabelId)),
                                                nullptr, nullptr);
        g_minimizeToTrayCheck = CreateWindowExW(0, L"BUTTON", L"",
                                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                12, 0, 20, 20, hwnd,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMinimizeToTrayCheckId)),
                                                nullptr, nullptr);
        if (g_minimizeToTrayLabel) {
            SendMessageW(g_minimizeToTrayLabel, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
        if (g_minimizeToTrayCheck) {
            SendMessageW(g_minimizeToTrayCheck, BM_SETCHECK,
                         g_state.minimizeToTrayOnClose ? BST_CHECKED : BST_UNCHECKED, 0);
        }

        g_linkIsSysLink = true;
        const wchar_t *linkText = ui_text::UiText(UiTextId::LinkMarkup).c_str();
        g_link = CreateWindowExW(0, WC_LINK, linkText,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 12, 124, 500, 24,
                                 hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLinkId)), nullptr, nullptr);
        if (!g_link) {
            // Fallback for systems without SysLink (e.g., missing comctl32 v6 manifest).
            g_linkIsSysLink = false;
            const wchar_t *fallbackText = ui_text::UiText(UiTextId::LinkPlain).c_str();
            g_link = CreateWindowExW(0, L"STATIC", fallbackText,
                                     WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                     12, 124, 500, 20,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLinkId)), nullptr, nullptr);
        }
        if (g_link) {
            SendMessageW(g_link, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
        g_hotkeyLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelHotkey).c_str(),
                                        WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                        12, 124, 60, 24, hwnd, nullptr, nullptr, nullptr);
        g_infoLabel = CreateWindowExW(0, L"STATIC", ui_text::UiText(UiTextId::LabelInfo).c_str(),
                                      WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                      12, 124, 40, 24, hwnd, nullptr, nullptr, nullptr);
        if (g_infoLabel) {
            SendMessageW(g_infoLabel, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
        if (g_hotkeyLabel) {
            if (g_hotkeyFont) {
                DeleteObject(g_hotkeyFont);
                g_hotkeyFont = nullptr;
            }
            g_hotkeyFont = createHotkeyFont(g_processLabel ? g_processLabel : g_hotkeyLabel);
            HFONT labelFont = g_hotkeyFont;
            if (!labelFont) {
                labelFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            }
            SendMessageW(g_hotkeyLabel, WM_SETFONT, reinterpret_cast<WPARAM>(labelFont), TRUE);
            if (g_infoLabel) {
                SendMessageW(g_infoLabel, WM_SETFONT, reinterpret_cast<WPARAM>(labelFont), TRUE);
            }
            if (g_minimizeToTrayLabel) {
                SendMessageW(g_minimizeToTrayLabel, WM_SETFONT, reinterpret_cast<WPARAM>(labelFont), TRUE);
            }
        }

        layoutControls(hwnd);
        refreshProcessList(combo, GetDlgItem(hwnd, kStatusLabelId));
        updateAutoHookCheckbox(hwnd);
        updateProcessBgmCheckbox(hwnd);
        updateMinimizeToTrayControls(hwnd);
        updateHookButtonState(hwnd);
        initKnownPids();
        scheduleAutoHooksForProcesses(hwnd, controller::enumerateSessionProcesses(), true);
        SetTimer(hwnd, kAutoHookTimerId, kAutoHookIntervalMs, nullptr);
        registerControllerHotkeys(hwnd, GetDlgItem(hwnd, kStatusLabelId));

        g_tooltip = createTooltip(hwnd);
        addTooltip(g_tooltip, combo, UiTextId::TooltipProcessCombo);
        addTooltip(g_tooltip, g_processLabel, UiTextId::TooltipProcessCombo);
        addTooltip(g_tooltip, refresh, UiTextId::TooltipHookButton);
        addTooltip(g_tooltip, speedEdit, UiTextId::TooltipSpeedEdit);
        addTooltip(g_tooltip, g_speedLabel, UiTextId::TooltipSpeedEdit);
        addTooltip(g_tooltip, ignoreBgm, UiTextId::TooltipProcessBgm);
        addTooltip(g_tooltip, g_ignoreBgmLabel, UiTextId::TooltipProcessBgm);
        addTooltip(g_tooltip, g_autoHookCheck, UiTextId::TooltipAutoHook);
        addTooltip(g_tooltip, g_autoHookLabel, UiTextId::TooltipAutoHook);
        addTooltip(g_tooltip, g_autoHookDelayCheck, UiTextId::TooltipAutoHookDelay);
        addTooltip(g_tooltip, g_autoHookDelayLabel, UiTextId::TooltipAutoHookDelay);
        addTooltip(g_tooltip, g_infoLabel, UiTextId::TooltipInfo);
        addTooltip(g_tooltip, g_hotkeyLabel, UiTextId::TooltipHotkey);
        // Hook + Apply tooltip removed with button.

        refreshUiText(hwnd);

        if (!g_initialOptions.launchPath.empty()) {
            handleLaunch(hwnd);
        }

        // Auto search & hook if requested via CLI.
        if (!g_state.searchTerm.empty()) {
            auto toLower = [](std::wstring s) {
                std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                return s;
            };
            const auto needle = toLower(g_state.searchTerm);
            int bestIdx = -1;
            size_t bestLen = std::numeric_limits<size_t>::max();
            for (size_t i = 0; i < g_state.processes.size(); ++i) {
                auto nameLower = toLower(g_state.processes[i].name);
                if (nameLower.find(needle) != std::wstring::npos) {
                    const size_t len = g_state.processes[i].name.length();
                    if (len < bestLen) {
                        bestLen = len;
                        bestIdx = static_cast<int>(i);
                    }
                }
            }

            HWND statusLabel = GetDlgItem(hwnd, kStatusLabelId);
            if (bestIdx >= 0) {
                SendMessageW(combo, CB_SETCURSEL, bestIdx, 0);
                updateAutoHookCheckbox(hwnd);
                updateProcessBgmCheckbox(hwnd);
                updateHookButtonState(hwnd);
                const auto &p = g_state.processes[static_cast<size_t>(bestIdx)];
                std::wstring msg = L"Auto-selected [" + std::to_wstring(p.pid) + L"] " + p.name + L" via --search \"" + g_state.searchTerm + L"\"";
                setStatus(statusLabel, msg);
                KRKR_LOG_INFO(L"Auto search hit: " + p.name);
                handleApply(hwnd);
            } else {
                std::wstring msg = L"--search \"" + g_state.searchTerm + L"\": no process matched; waiting for manual selection.";
                setStatus(statusLabel, msg);
                KRKR_LOG_INFO(L"Search term not found: " + g_state.searchTerm);
            }
        }
        break;
    }
    case WM_SIZE:
        layoutControls(hwnd);
        break;
    case WM_COMMAND: {
        const WORD id = LOWORD(wParam);
        if (id == kRefreshButtonId && HIWORD(wParam) == BN_CLICKED) {
            handleApply(hwnd);
        } else if (id == kProcessComboId && HIWORD(wParam) == CBN_DROPDOWN) {
            refreshProcessUi(hwnd, GetDlgItem(hwnd, kProcessComboId), GetDlgItem(hwnd, kStatusLabelId));
        } else if (id == kProcessComboId && HIWORD(wParam) == CBN_SELCHANGE) {
            updateAutoHookCheckbox(hwnd);
            updateProcessBgmCheckbox(hwnd);
            updateHookButtonState(hwnd);
        } else if (id == kLanguageComboId && HIWORD(wParam) == CBN_SELCHANGE) {
            const int sel = static_cast<int>(SendMessageW(g_languageCombo, CB_GETCURSEL, 0, 0));
            if (sel == 1) {
                ui_text::SetUiLanguage(L"zh-CN");
            } else if (sel == 2) {
                ui_text::SetUiLanguage(L"ja");
            } else {
                ui_text::SetUiLanguage(L"en");
            }
            refreshUiText(hwnd);
        } else if (id == kSpeedEditId && HIWORD(wParam) == EN_KILLFOCUS) {
            HWND editSpeed = GetDlgItem(hwnd, kSpeedEditId);
            if (editSpeed) {
                readSpeedFromEdit(editSpeed);
                writeSpeedEdit(hwnd);
                applySpeedToHookedProcesses(hwnd);
            }
        } else if (id == kIgnoreBgmCheckId && HIWORD(wParam) == BN_CLICKED) {
            syncProcessAllAudioFromCheckbox(hwnd);
            handleProcessBgmToggle(hwnd);
            applySettingsToSelectedIfHooked(hwnd);
        } else if (id == kIgnoreBgmLabelId && HIWORD(wParam) == STN_CLICKED) {
            toggleCheckboxFromLabel(hwnd, kIgnoreBgmCheckId);
        } else if (id == kAutoHookCheckId && HIWORD(wParam) == BN_CLICKED) {
            handleAutoHookToggle(hwnd);
        } else if (id == kAutoHookLabelId && HIWORD(wParam) == STN_CLICKED) {
            toggleCheckboxFromLabel(hwnd, kAutoHookCheckId);
        } else if (id == kAutoHookDelayCheckId && HIWORD(wParam) == BN_CLICKED) {
            handleAutoHookDelayToggle(hwnd);
        } else if (id == kAutoHookDelayLabelId && HIWORD(wParam) == STN_CLICKED) {
            toggleCheckboxFromLabel(hwnd, kAutoHookDelayCheckId);
        } else if (id == kMinimizeToTrayCheckId && HIWORD(wParam) == BN_CLICKED) {
            handleMinimizeToTrayToggle(hwnd);
        } else if (id == kMinimizeToTrayLabelId && HIWORD(wParam) == STN_CLICKED) {
            toggleCheckboxFromLabel(hwnd, kMinimizeToTrayCheckId);
        } else if (id == kLinkId && HIWORD(wParam) == STN_CLICKED) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/caca2331/kirikiri-speed-controller", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == kTrayMenuExitId) {
            g_allowClose = true;
            hideTrayIcon();
            DestroyWindow(hwnd);
        }
        break;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        HWND speedEdit = GetDlgItem(hwnd, kSpeedEditId);
        if (speedEdit && GetFocus() == speedEdit) {
            SetFocus(hwnd);
        }
        break;
    }
    case WM_TIMER:
        if (wParam == kAutoHookTimerId) {
            pollAutoHook(hwnd);
            updateProcessBgmCheckbox(hwnd);
            return 0;
        }
        if (wParam == kLaunchRefreshTimerId) {
            handleLaunchRefreshTick(hwnd);
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (wParam == kHotkeyToggleSpeedId) {
            handleSpeedHotkey(hwnd, SpeedHotkeyAction::Toggle);
        } else if (wParam == kHotkeySpeedUpId) {
            handleSpeedHotkey(hwnd, SpeedHotkeyAction::SpeedUp);
        } else if (wParam == kHotkeySpeedDownId) {
            handleSpeedHotkey(hwnd, SpeedHotkeyAction::SpeedDown);
        } else {
            break;
        }
        return 0;
    case kMsgStatusUpdate: {
        std::unique_ptr<std::wstring> msg(reinterpret_cast<std::wstring *>(lParam));
        if (msg) {
            setStatus(GetDlgItem(hwnd, kStatusLabelId), *msg);
        }
        return 0;
    }
    case kMsgInjectionDone: {
        std::unique_ptr<InjectionResult> result(reinterpret_cast<InjectionResult *>(lParam));
        if (result) {
            handleInjectionDone(hwnd, *result);
        }
        return 0;
    }
    case kMsgLaunchDone: {
        std::unique_ptr<InjectionResult> result(reinterpret_cast<InjectionResult *>(lParam));
        if (result) {
            handleLaunchDone(hwnd, *result);
        }
        return 0;
    }
    case kMsgAutoSelectPid:
        handleAutoSelectPid(hwnd, static_cast<DWORD>(wParam));
        return 0;
    case kTrayCallbackMsg: {
        switch (lParam) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            restoreFromTray(hwnd);
            return 0;
        case WM_RBUTTONUP:
            showTrayMenu(hwnd);
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_NOTIFY: {
        auto *hdr = reinterpret_cast<NMHDR *>(lParam);
        if (hdr && hdr->idFrom == kLinkId && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/caca2331/kirikiri-speed-controller", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (!g_allowClose && g_state.minimizeToTrayOnClose) {
            showTrayIcon(hwnd);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        hideTrayIcon();
        KillTimer(hwnd, kAutoHookTimerId);
        KillTimer(hwnd, kLaunchRefreshTimerId);
        UnregisterHotKey(hwnd, kHotkeyToggleSpeedId);
        UnregisterHotKey(hwnd, kHotkeySpeedUpId);
        UnregisterHotKey(hwnd, kHotkeySpeedDownId);
        g_activeTooltipControl = nullptr;
        g_mainWindow = nullptr;
        if (g_hotkeyFont) {
            DeleteObject(g_hotkeyFont);
            g_hotkeyFont = nullptr;
        }
        KRKR_LOG_INFO("KrkrSpeedController window destroyed, exiting.");
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

void setInitialOptions(const ControllerOptions &opts) {
    g_initialOptions = opts;
    g_state.enableLog = opts.enableLog;
    g_state.processAllAudio = opts.processAllAudio;
    g_state.minimizeToTrayOnClose = controller::getMinimizeToTrayOnClose();
    controller::initSpeedState(g_state.speed, opts.speed, true);
    g_state.bgmSeconds = opts.bgmSeconds;
    g_state.launchPath = opts.launchPath.empty() ? std::filesystem::path{} : std::filesystem::path(opts.launchPath);
    g_state.stereoBgmMode = opts.stereoBgmMode;
    g_state.searchTerm = opts.searchTerm;
}

ControllerOptions getInitialOptions() {
    return g_initialOptions;
}

int runController(HINSTANCE hInstance, int nCmdShow) {
    KRKR_LOG_INFO("KrkrSpeedController GUI starting");
    const wchar_t *CLASS_NAME = kControllerWindowClass;

    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES | ICC_LINK_CLASS};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));

    RegisterClassExW(&wc);

    ensureUiTextLoaded();
    const auto &title = ui_text::UiText(UiTextId::WindowTitle);
    HWND hwnd = CreateWindowExW(0, CLASS_NAME, title.c_str(),
                                WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 620, 240,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        KRKR_LOG_ERROR("Failed to create main window");
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        updateTrackedTooltip(msg);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace krkrspeed::ui
