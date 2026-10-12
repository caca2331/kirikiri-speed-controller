#include "ui_internal.h"
#include "resource.h"

#include "../common/Logging.h"
#include <algorithm>
#include <cstring>
#include <iterator>

namespace krkrspeed::ui::detail {
namespace {
const std::wstring &resolveTooltipText(UiTextId id);

TOOLINFOW makeToolInfo(HWND control, UiTextId textId) {
    TOOLINFOW ti{};
    #ifdef TTTOOLINFOW_V1_SIZE
    ti.cbSize = TTTOOLINFOW_V1_SIZE;
    #else
    ti.cbSize = sizeof(ti);
    #endif
    ti.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = GetParent(control);
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    ti.lpszText = const_cast<wchar_t *>(resolveTooltipText(textId).c_str());
    return ti;
}

const std::wstring &resolveTooltipText(UiTextId id) {
    if (id != UiTextId::TooltipInfo) {
        return ui_text::UiText(id);
    }
    static std::wstring cached;
    cached = ui_text::UiText(UiTextId::TooltipInfo);
    if (cached.empty()) {
        return cached;
    }
    const std::wstring needle = L"<version>";
    std::size_t pos = 0;
    while ((pos = cached.find(needle, pos)) != std::wstring::npos) {
        cached.replace(pos, needle.size(), kAppVersion);
        pos += kAppVersion.size();
    }
    return cached;
}

bool registerHotkey(HWND hwnd, int id, UINT vk) {
    if (!RegisterHotKey(hwnd, id, kHotkeyModifiers, vk)) {
        KRKR_LOG_WARN("RegisterHotKey failed id=" + std::to_string(id) + " err=" +
                      std::to_string(GetLastError()));
        return false;
    }
    return true;
}

} // namespace

void setStatus(HWND statusLabel, const std::wstring &text) {
    if (!statusLabel) return;
    if (!g_statusBuffer.empty()) {
        g_statusBuffer += L"\r\n";
    }
    g_statusBuffer += text;
    if (g_statusBuffer.size() > kStatusMaxChars) {
        const std::size_t excess = g_statusBuffer.size() - kStatusMaxChars;
        std::size_t trimPos = g_statusBuffer.find(L'\n', excess);
        if (trimPos != std::wstring::npos && trimPos + 1 < g_statusBuffer.size()) {
            g_statusBuffer.erase(0, trimPos + 1);
        } else {
            g_statusBuffer.erase(0, excess);
        }
    }
    SetWindowTextW(statusLabel, g_statusBuffer.c_str());
    scrollStatusToBottom(statusLabel);
    KRKR_LOG_INFO(text);
}

void scrollStatusToBottom(HWND statusLabel) {
    if (!statusLabel) return;
    const int length = GetWindowTextLengthW(statusLabel);
    SendMessageW(statusLabel, EM_SETSEL, length, length);
    SendMessageW(statusLabel, EM_SCROLLCARET, 0, 0);
    SendMessageW(statusLabel, WM_VSCROLL, SB_BOTTOM, 0);
}

void toggleCheckboxFromLabel(HWND hwnd, int checkId) {
    if (!hwnd) return;
    HWND check = GetDlgItem(hwnd, checkId);
    if (!check || !IsWindowEnabled(check)) return;
    SendMessageW(check, BM_CLICK, 0, 0);
}

void ensureUiTextLoaded() {
    static bool loaded = false;
    if (loaded) return;
    std::wstring error;
    auto path = controller::controllerDirectory() / L"ui_texts.yaml";
    if (!ui_text::LoadUiTextPacks(path, error)) {
        if (!error.empty()) {
            KRKR_LOG_WARN(L"UiText load failed: " + error);
        }
    }
    const auto saved = controller::getUiLanguage();
    g_languageIndex = kDefaultLanguageIndex;
    for (int i = 0; i < static_cast<int>(std::size(kLanguageCodes)); ++i) {
        if (!saved.empty() && _wcsicmp(saved.c_str(), kLanguageCodes[i]) == 0) {
            g_languageIndex = i;
        }
    }
    ui_text::SetUiLanguage(kLanguageCodes[g_languageIndex]);
    loaded = true;
}

// Switches the UI text to a language combo entry and remembers the choice in the config file.
void selectUiLanguage(HWND hwnd, int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kLanguageCodes))) {
        index = 0;
    }
    g_languageIndex = index;
    ui_text::SetUiLanguage(kLanguageCodes[index]);
    refreshUiText(hwnd);
    std::wstring error;
    if (!controller::setUiLanguage(kLanguageCodes[index], error)) {
        KRKR_LOG_WARN(L"Failed to save UI language: " + error);
    }
}

void refreshUiText(HWND hwnd) {
    if (!hwnd) return;
    if (g_processLabel) {
        SetWindowTextW(g_processLabel, ui_text::UiText(UiTextId::LabelProcess).c_str());
    }
    if (g_speedLabel) {
        SetWindowTextW(g_speedLabel, ui_text::UiText(UiTextId::LabelSpeed).c_str());
    }
    if (g_ignoreBgmLabel) {
        SetWindowTextW(g_ignoreBgmLabel, ui_text::UiText(UiTextId::LabelProcessBgm).c_str());
    }
    if (g_autoHookLabel) {
        SetWindowTextW(g_autoHookLabel, ui_text::UiText(UiTextId::LabelAutoHook).c_str());
    }
    if (g_autoHookDelayLabel) {
        SetWindowTextW(g_autoHookDelayLabel, ui_text::UiText(UiTextId::LabelAutoHookDelay).c_str());
    }
    if (g_hotkeyLabel) {
        SetWindowTextW(g_hotkeyLabel, ui_text::UiText(UiTextId::LabelHotkey).c_str());
    }
    if (g_infoLabel) {
        SetWindowTextW(g_infoLabel, ui_text::UiText(UiTextId::LabelInfo).c_str());
    }
    if (g_minimizeToTrayLabel) {
        SetWindowTextW(g_minimizeToTrayLabel, ui_text::UiText(UiTextId::LabelMinimizeToTray).c_str());
    }
    if (g_link) {
        const auto &linkText = g_linkIsSysLink ? ui_text::UiText(UiTextId::LinkMarkup)
                                               : ui_text::UiText(UiTextId::LinkPlain);
        SetWindowTextW(g_link, linkText.c_str());
    }
    SetWindowTextW(hwnd, ui_text::UiText(UiTextId::WindowTitle).c_str());

    updateHookButtonState(hwnd);
    updateTooltips();

}

void layoutControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int padding = 12;
    const int labelWidth = 90;
    const int labelGap = 5;
    const int labelColumn = labelWidth + labelGap;
    const int comboHeight = 24;
    const int editWidth = 40;
    const int buttonWidth = 120;
    const int rowHeight = 28;
    const int checkboxHeight = 20;
    const int checkboxWidth = 20;
    const int statusHeight = comboHeight * 3;

    int x = padding;
    int y = padding;

    int comboWidth = static_cast<int>(rc.right) - labelColumn - buttonWidth - padding * 3 + 5;
    if (comboWidth < 0) {
        comboWidth = 0;
    }
    SetWindowPos(GetDlgItem(hwnd, kProcessComboId), nullptr, x + labelColumn, y, comboWidth, comboHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kRefreshButtonId), nullptr, rc.right - buttonWidth - padding, y, buttonWidth, comboHeight,
                 SWP_NOZORDER);
    if (g_processLabel) {
        SetWindowPos(g_processLabel, nullptr, x, y + 2, labelWidth, comboHeight, SWP_NOZORDER);
    }

    y += rowHeight;
    if (g_speedLabel) {
        SetWindowPos(g_speedLabel, nullptr, x, y + 2, labelWidth, comboHeight, SWP_NOZORDER);
    }
    SetWindowPos(GetDlgItem(hwnd, kSpeedEditId), nullptr, x + labelColumn, y, editWidth, comboHeight, SWP_NOZORDER);
    // Place ignore-BGM label and checkbox together
    SetWindowPos(GetDlgItem(hwnd, kIgnoreBgmLabelId), nullptr, x + labelColumn + editWidth + padding, y + 2, 90, checkboxHeight,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kIgnoreBgmCheckId), nullptr, x + labelColumn + editWidth + padding + 94, y, checkboxWidth, checkboxHeight,
                 SWP_NOZORDER);
    if (g_autoHookLabel) {
        const int autoHookGap = 10;
        const int innerGap = 6;
        const int delayGap = 10;
        const int autoHookX = x + labelColumn + editWidth + padding + 94 + checkboxWidth + autoHookGap;
        const int autoHookLabelWidth = 130;
        SetWindowPos(g_autoHookLabel, nullptr, autoHookX, y + 2, autoHookLabelWidth, checkboxHeight, SWP_NOZORDER);
        if (g_autoHookCheck) {
            const int autoHookCheckX = autoHookX + autoHookLabelWidth + innerGap;
            SetWindowPos(g_autoHookCheck, nullptr, autoHookCheckX, y, checkboxWidth, checkboxHeight, SWP_NOZORDER);
            if (g_autoHookDelayLabel) {
                const int delayLabelWidth = 125;
                const int delayCheckX = rc.right - padding - checkboxWidth;
                const int delayLabelX = delayCheckX - delayGap - delayLabelWidth;
                SetWindowPos(g_autoHookDelayLabel, nullptr, delayLabelX, y + 2, delayLabelWidth, checkboxHeight, SWP_NOZORDER);
                if (g_autoHookDelayCheck) {
                    SetWindowPos(g_autoHookDelayCheck, nullptr, delayCheckX, y, checkboxWidth, checkboxHeight, SWP_NOZORDER);
                }
            }
        }
    }
    // Hook + Apply button removed; "Hook" button handles injection.

    y += rowHeight;
    SetWindowPos(GetDlgItem(hwnd, kStatusLabelId), nullptr, x, y, rc.right - padding * 2, statusHeight, SWP_NOZORDER);

    const int trayRowY = y + statusHeight + 6;
    if (g_minimizeToTrayLabel && g_minimizeToTrayCheck) {
        int trayLabelWidth = rc.right - padding * 2 - checkboxWidth - 6;
        trayLabelWidth = std::min(trayLabelWidth, 180);
        if (trayLabelWidth < 80) trayLabelWidth = 80;
        SetWindowPos(g_minimizeToTrayLabel, nullptr, x, trayRowY + 2, trayLabelWidth, checkboxHeight, SWP_NOZORDER);
        SetWindowPos(g_minimizeToTrayCheck, nullptr, x + trayLabelWidth + 6, trayRowY, checkboxWidth, checkboxHeight,
                     SWP_NOZORDER);
    }

    const int linkPadding = 8;
    const int comboX = rc.right - buttonWidth - padding;
    const int comboY = rc.bottom - padding - comboHeight;
    const int hotkeyWidth = static_cast<int>(buttonWidth * 0.8f);
    const int infoWidth = std::max(24, hotkeyWidth / 3);
    const int hotkeyX = comboX - linkPadding - hotkeyWidth;
    const int infoX = hotkeyX - linkPadding - infoWidth;
    int linkWidth = infoX - x - linkPadding;
    if (linkWidth < 0) linkWidth = 0;

    int textHeight = 0;
    if (g_link) {
        textHeight = std::max(textHeight, getTextHeight(g_link, comboHeight));
    }
    if (g_hotkeyLabel) {
        textHeight = std::max(textHeight, getTextHeight(g_hotkeyLabel, comboHeight));
    }
    if (textHeight <= 0) {
        textHeight = comboHeight;
    }
    const int textBottomY = rc.bottom - padding - textHeight;

    if (g_link) {
        SetWindowPos(g_link, nullptr, x, textBottomY, linkWidth, textHeight, SWP_NOZORDER);
    }
    if (g_infoLabel) {
        SetWindowPos(g_infoLabel, nullptr, infoX, textBottomY, infoWidth, textHeight, SWP_NOZORDER);
    }
    if (g_hotkeyLabel) {
        SetWindowPos(g_hotkeyLabel, nullptr, hotkeyX, textBottomY, hotkeyWidth, textHeight, SWP_NOZORDER);
    }
    if (g_languageCombo) {
        const int dropHeight = comboHeight * 4; // selection + 3 items
        SetWindowPos(g_languageCombo, nullptr, comboX, comboY, buttonWidth, dropHeight, SWP_NOZORDER);
    }
}

int getTextHeight(HWND hwnd, int fallback) {
    if (!hwnd) return fallback;
    HDC hdc = GetDC(hwnd);
    if (!hdc) return fallback;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font) {
        oldFont = SelectObject(hdc, font);
    }
    TEXTMETRICW tm{};
    int height = fallback;
    if (GetTextMetricsW(hdc, &tm)) {
        height = tm.tmHeight + tm.tmExternalLeading;
    }
    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    ReleaseDC(hwnd, hdc);
    return height;
}

HFONT createHotkeyFont(HWND reference) {
    if (!reference) return nullptr;
    HFONT baseFont = reinterpret_cast<HFONT>(SendMessageW(reference, WM_GETFONT, 0, 0));
    if (!baseFont) {
        baseFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    LOGFONTW lf{};
    if (baseFont && GetObjectW(baseFont, sizeof(lf), &lf) != 0) {
        HDC hdc = GetDC(reference);
        const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc) {
            ReleaseDC(reference, hdc);
        }
        const int delta = MulDiv(2, dpi, 72);
        int height = lf.lfHeight;
        if (height == 0) {
            height = -MulDiv(11, dpi, 72);
        } else {
            if (height < 0) height = -height;
            height += delta;
            height = -height;
        }
        lf.lfHeight = height;
        lf.lfWeight = FW_BOLD;
        return CreateFontIndirectW(&lf);
    }
    return nullptr;
}

HWND createTooltip(HWND parent) {
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (tooltip) {
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(tooltip, TTM_ACTIVATE, TRUE, 0);
        SetWindowPos(tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        KRKR_LOG_WARN("Tooltip create failed err=" + std::to_string(GetLastError()));
    }
    return tooltip;
}

void addTooltip(HWND tooltip, HWND control, UiTextId textId) {
    if (!tooltip || !control) return;

    const auto key = reinterpret_cast<std::uintptr_t>(control);
    g_tooltipById[key] = textId;

    TOOLINFOW ti = makeToolInfo(control, textId);
    if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti))) {
        KRKR_LOG_WARN("Tooltip add failed for control=" + std::to_string(key) +
                      " err=" + std::to_string(GetLastError()));
    }
}

bool setTooltipId(HWND control, UiTextId textId) {
    if (!control) return false;
    const auto key = reinterpret_cast<std::uintptr_t>(control);
    auto it = g_tooltipById.find(key);
    if (it != g_tooltipById.end() && it->second == textId) {
        return false;
    }
    g_tooltipById[key] = textId;
    return true;
}

void updateTooltips() {
    if (!g_tooltip) return;
    for (const auto &kv : g_tooltipById) {
        HWND control = reinterpret_cast<HWND>(kv.first);
        if (!IsWindow(control)) {
            continue;
        }
        TOOLINFOW ti = makeToolInfo(control, kv.second);
        SendMessageW(g_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

void updateTrackedTooltip(const MSG &msg) {
    if (!g_tooltip || msg.hwnd == g_tooltip) return;
    if (msg.message == WM_MOUSELEAVE) {
        if (g_activeTooltipControl && msg.hwnd == g_activeTooltipControl) {
            auto it = g_tooltipById.find(reinterpret_cast<std::uintptr_t>(g_activeTooltipControl));
            if (it != g_tooltipById.end()) {
                TOOLINFOW ti = makeToolInfo(g_activeTooltipControl, it->second);
                SendMessageW(g_tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
            }
            g_activeTooltipControl = nullptr;
        }
        return;
    }
    if (msg.message < WM_MOUSEFIRST || msg.message > WM_MOUSELAST) {
        return;
    }

    HWND target = nullptr;
    if (g_mainWindow) {
        HWND hit = WindowFromPoint(msg.pt);
        if (hit == g_tooltip) {
            hit = nullptr;
        }
        if (!hit || hit == g_mainWindow) {
            POINT clientPt = msg.pt;
            ScreenToClient(g_mainWindow, &clientPt);
            hit = ChildWindowFromPointEx(g_mainWindow, clientPt, CWP_SKIPINVISIBLE);
            if (hit == g_mainWindow) {
                hit = nullptr;
            }
        }
        target = hit ? hit : msg.hwnd;
    } else {
        target = msg.hwnd;
    }
    while (target && target != g_mainWindow) {
        if (g_tooltipById.find(reinterpret_cast<std::uintptr_t>(target)) != g_tooltipById.end()) {
            break;
        }
        target = GetParent(target);
    }
    if (target == g_mainWindow) {
        target = nullptr;
    }

    if (target != g_activeTooltipControl) {
        if (g_activeTooltipControl) {
            auto itPrev = g_tooltipById.find(reinterpret_cast<std::uintptr_t>(g_activeTooltipControl));
            if (itPrev != g_tooltipById.end()) {
                TOOLINFOW ti = makeToolInfo(g_activeTooltipControl, itPrev->second);
                SendMessageW(g_tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
            }
        }
        g_activeTooltipControl = target;
        if (!g_activeTooltipControl) {
            return;
        }
    }

    if (g_activeTooltipControl) {
        auto it = g_tooltipById.find(reinterpret_cast<std::uintptr_t>(g_activeTooltipControl));
        if (it == g_tooltipById.end()) {
            return;
        }
        TOOLINFOW ti = makeToolInfo(g_activeTooltipControl, it->second);
        SendMessageW(g_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
        const int x = msg.pt.x + 12;
        const int y = msg.pt.y + 18;
        SendMessageW(g_tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
        SendMessageW(g_tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));

        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = g_activeTooltipControl;
        TrackMouseEvent(&tme);
    }
}

void showTrayIcon(HWND hwnd) {
    if (!hwnd || g_trayIconVisible) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
    if (!icon) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    }
    if (!icon) {
        icon = LoadIconW(nullptr, MAKEINTRESOURCEW(IDI_APPLICATION));
    }
    nid.hIcon = icon;
    const auto &title = ui_text::UiText(UiTextId::WindowTitle);
    if (!title.empty()) {
        wcsncpy_s(nid.szTip, title.c_str(), _TRUNCATE);
    }
    Shell_NotifyIconW(NIM_ADD, &nid);
    g_trayIcon = nid;
    g_trayIconVisible = true;
}

void hideTrayIcon() {
    if (!g_trayIconVisible) return;
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_trayIconVisible = false;
    std::memset(&g_trayIcon, 0, sizeof(g_trayIcon));
}

void restoreFromTray(HWND hwnd) {
    if (!hwnd) return;
    hideTrayIcon();
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

void showTrayMenu(HWND hwnd) {
    if (!hwnd) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kTrayMenuExitId, ui_text::UiText(UiTextId::MenuExit).c_str());
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

void registerControllerHotkeys(HWND hwnd, HWND statusLabel) {
    bool ok = true;
    ok &= registerHotkey(hwnd, kHotkeyToggleSpeedId, VK_OEM_7); // '
    ok &= registerHotkey(hwnd, kHotkeySpeedUpId, VK_OEM_6); // ]
    ok &= registerHotkey(hwnd, kHotkeySpeedDownId, VK_OEM_4); // [
    if (!ok && statusLabel) {
        setStatus(statusLabel, L"Failed to register one or more hotkeys.");
    }
}

} // namespace krkrspeed::ui::detail
