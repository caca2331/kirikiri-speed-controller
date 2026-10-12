#pragma once

#include "../common/SharedSettings.h"
#include <Windows.h>
#include <atomic>
#include <mutex>

namespace krkrspeed {

// Single owner of the controller's per-process settings block. Hooks read a sanitized snapshot
// instead of mapping the shared memory themselves.
class SharedSettingsManager {
public:
    static SharedSettingsManager &instance();

    // Maps Local\KrkrSpeedSettings_<pid> and applies it. Returns false when the controller has not created it.
    bool attachSharedSettings();
    // Re-reads the mapped block; while unmapped, retries attaching at most once per second.
    void pollSharedSettings();
    void applySharedSettings(const SharedSettings &settings);

    SharedSettings snapshot() const;
    float getUserSpeed() const;

private:
    SharedSettingsManager() = default;

    mutable std::mutex m_mutex;
    SharedSettings m_settings{};

    std::mutex m_attachMutex;
    HANDLE m_sharedMapping = nullptr;
    std::atomic<const SharedSettings *> m_sharedView{nullptr};
    std::atomic<std::uint64_t> m_nextAttachMs{0};
    std::atomic<bool> m_warnedMissingMap{false};
};

} // namespace krkrspeed
