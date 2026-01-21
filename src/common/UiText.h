#pragma once

#include <filesystem>
#include <string>

namespace krkrspeed::ui_text {

enum class UiTextId {
    WindowTitle,
    LabelProcess,
    ButtonHook,
    ButtonHooked,
    LabelSpeed,
    LabelProcessBgm,
    LabelAutoHook,
    LabelAutoHookDelay,
    LabelHotkey,
    LabelInfo,
    LabelMinimizeToTray,
    LinkMarkup,
    LinkPlain,
    MenuExit,
    TooltipProcessCombo,
    TooltipHookButton,
    TooltipSpeedEdit,
    TooltipSpeedEditWasapi,
    TooltipProcessBgm,
    TooltipProcessBgmWasapi,
    TooltipAutoHook,
    TooltipAutoHookDelay,
    TooltipHotkey,
    TooltipInfo
};

bool LoadUiTextPacks(const std::filesystem::path &path, std::wstring &error);
void SetUiLanguage(const std::wstring &langCode);
const std::wstring &UiText(UiTextId id);

} // namespace krkrspeed::ui_text
