#pragma once

#include <cstdint>
#include <string>

namespace krkrspeed {

struct SharedSettings {
    float userSpeed = 1.5f;
    float lengthGateSeconds = 60.0f;
    std::uint32_t lengthGateEnabled = 1;
    std::uint32_t enableLog = 0;
    std::uint32_t disableBgm = 0;
    std::uint32_t processAllAudio = 0;
    float bgmSecondsGate = 60.0f;
    std::uint32_t stereoBgmMode = 1; // 0=aggressive,1=hybrid(default),2=none
    // New fields go at the end only, so hooks and controllers of different versions still agree on the
    // fields they share. Both sides map the whole section, so an older (shorter) block reads these as 0.
    std::uint32_t layoutSize = 0;        // sizeof(SharedSettings) of the writer; 0 = controller without version tagging
    std::uint32_t controllerVersion = 0; // KRKR_VERSION_PACKED of the writer; 0 = controller without version tagging
};

inline std::wstring BuildSharedSettingsName(std::uint32_t pid) {
    return L"Local\\KrkrSpeedSettings_" + std::to_wstring(pid);
}

} // namespace krkrspeed
