#include "SharedSettingsManager.h"
#include "../common/Logging.h"
#include "KrkrVersion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace krkrspeed {

namespace {

std::string formatPackedVersion(std::uint32_t packed) {
    return std::to_string(packed / 10000) + "." + std::to_string(packed / 100 % 100) + "." + std::to_string(packed % 100);
}

} // namespace

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
        // Size 0 maps the whole section, which may be shorter than our struct if an older controller made it;
        // views are page-granular and pagefile sections start zeroed, so the missing tail reads as 0.
        view = static_cast<const SharedSettings *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
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
    if (snap.controllerVersion == 0) {
        KRKR_LOG_WARN("Attached to shared settings map; controller predates version tagging (hook " +
                      std::string(KRKR_VERSION_STRING) + ")");
    } else if (snap.controllerVersion != KRKR_VERSION_PACKED) {
        KRKR_LOG_WARN("Attached to shared settings map; controller " + formatPackedVersion(snap.controllerVersion) +
                      " differs from hook " + KRKR_VERSION_STRING + "; update both together");
    } else {
        KRKR_LOG_INFO("Attached to shared settings map (controller and hook " + std::string(KRKR_VERSION_STRING) + ")");
    }
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
