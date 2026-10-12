#include "SharedSettingsManager.h"
#include "../common/Logging.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace krkrspeed {

SharedSettingsManager &SharedSettingsManager::instance() {
    static SharedSettingsManager mgr;
    return mgr;
}

bool SharedSettingsManager::attachSharedSettings() {
    const SharedSettings *view = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_attachMutex);
        if (m_sharedView.load()) {
            return true;
        }
        const auto name = BuildSharedSettingsName(static_cast<std::uint32_t>(GetCurrentProcessId()));
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (!mapping) {
            if (!m_warnedMissingMap.exchange(true)) {
                KRKR_LOG_WARN("Shared settings map not found; using defaults");
            }
            return false;
        }
        view = static_cast<const SharedSettings *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedSettings)));
        if (!view) {
            KRKR_LOG_WARN("MapViewOfFile failed for shared settings");
            CloseHandle(mapping);
            return false;
        }
        m_sharedMapping = mapping;
        m_sharedView.store(view);
    }
    SharedSettings snap{};
    std::memcpy(&snap, view, sizeof(snap));
    applySharedSettings(snap);
    KRKR_LOG_INFO("Attached to shared settings map");
    return true;
}

void SharedSettingsManager::pollSharedSettings() {
    const SharedSettings *view = m_sharedView.load();
    if (!view) {
        const std::uint64_t now = GetTickCount64();
        if (now < m_nextAttachMs.load()) {
            return;
        }
        m_nextAttachMs.store(now + 1000);
        attachSharedSettings();
        return;
    }
    SharedSettings snap{};
    std::memcpy(&snap, view, sizeof(snap));
    applySharedSettings(snap);
}

void SharedSettingsManager::applySharedSettings(const SharedSettings &settings) {
    SharedSettings next = settings;
    next.userSpeed = std::isfinite(settings.userSpeed) ? std::clamp(settings.userSpeed, 0.5f, 10.0f) : 1.0f;
    next.lengthGateSeconds = std::isfinite(settings.lengthGateSeconds)
        ? std::clamp(settings.lengthGateSeconds, 0.1f, 600.0f) : 60.0f;
    next.bgmSecondsGate = std::isfinite(settings.bgmSecondsGate)
        ? std::clamp(settings.bgmSecondsGate, 0.1f, 600.0f) : 60.0f;
    if (next.stereoBgmMode > 2) {
        next.stereoBgmMode = 1;
    }
    if (next.enableLog) {
        SetLoggingEnabled(true);
    }

    SharedSettings prev{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        prev = m_settings;
        m_settings = next;
    }

    if (std::fabs(next.userSpeed - prev.userSpeed) > 0.001f) {
        KRKR_LOG_INFO("Shared speed updated to " + std::to_string(next.userSpeed) + "x");
    }
    if ((next.lengthGateEnabled != 0) != (prev.lengthGateEnabled != 0) ||
        std::fabs(next.lengthGateSeconds - prev.lengthGateSeconds) > 0.001f) {
        KRKR_LOG_INFO(std::string("Shared length gate ") + (next.lengthGateEnabled ? "enabled" : "disabled") +
                      " @ " + std::to_string(next.lengthGateSeconds) + "s");
    }
    if (next.processAllAudio != prev.processAllAudio || next.disableBgm != prev.disableBgm ||
        next.stereoBgmMode != prev.stereoBgmMode || std::fabs(next.bgmSecondsGate - prev.bgmSecondsGate) > 1e-4f) {
        KRKR_LOG_INFO(std::string("Shared settings: processAllAudio=") + (next.processAllAudio ? "1" : "0") +
                      " disableBgm=" + (next.disableBgm ? "1" : "0") +
                      " gate=" + std::to_string(next.bgmSecondsGate) +
                      " stereoMode=" + std::to_string(next.stereoBgmMode));
    }
}

SharedSettings SharedSettingsManager::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
}

float SharedSettingsManager::getUserSpeed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings.userSpeed;
}

} // namespace krkrspeed
