#include "AudioStreamProcessor.h"
#include "Logging.h"

#include <algorithm>

namespace krkrspeed {

AudioStreamProcessor::AudioStreamProcessor(std::uint32_t sampleRate, std::uint32_t channels, std::uint32_t blockAlign,
                                           const DspConfig &cfg)
    : m_sampleRate(sampleRate), m_blockAlign(blockAlign) {
    m_dsp = std::make_unique<DspPipeline>(sampleRate, channels, cfg);
    if (m_blockAlign == 0 && channels > 0) {
        m_blockAlign = channels * sizeof(std::int16_t);
    }
}

AudioProcessResult AudioStreamProcessor::process(const std::uint8_t *data, std::size_t bytes, float userSpeed,
                                                 bool shouldLog, std::uintptr_t key) {
    AudioProcessResult result;
    if (!data || bytes == 0 || !m_dsp) {
        result.output.assign(data, data + bytes);
        result.cbufferSize = m_cbuffer.size();
        result.appliedSpeed = 1.0f;
        return result;
    }

    const float denom = std::max(0.01f, userSpeed);
    const float pitchDown = 1.0f / denom;

    std::vector<std::uint8_t> streamIn;
    streamIn.reserve(m_cbuffer.size() + bytes);
    streamIn.insert(streamIn.end(), m_cbuffer.begin(), m_cbuffer.end());
    streamIn.insert(streamIn.end(), data, data + bytes);
    m_cbuffer.clear();

    auto out = m_dsp->process(streamIn.data(), streamIn.size(), pitchDown, DspMode::Pitch);
    if (out.empty()) {
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: pitch-compensate produced 0 bytes; passthrough key=" +
                           std::to_string(key));
        }
        result.output.assign(data, data + bytes);
        result.cbufferSize = m_cbuffer.size();
        result.appliedSpeed = userSpeed;
        m_lastAppliedSpeed = result.appliedSpeed;
        return result;
    }

    if (out.size() > bytes) {
        result.output.resize(bytes);
        std::copy_n(out.data(), bytes, result.output.begin());
        std::size_t excess = out.size() - bytes;
        m_cbuffer.insert(m_cbuffer.end(), out.data() + bytes, out.data() + out.size());
    } else if (out.size() < bytes) {
        const std::size_t pad = bytes - out.size();
        result.output.resize(bytes);
        std::fill(result.output.begin(), result.output.begin() + pad, 0);
        std::copy(out.begin(), out.end(), result.output.begin() + pad);
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: zero-padded " + std::to_string(pad) +
                           " bytes (pitch) key=" + std::to_string(key));
        }
    } else {
        result.output = std::move(out);
    }

    const std::size_t bytesPerSec = std::max<std::size_t>(1, m_blockAlign * m_sampleRate);
    const std::size_t cap =
        std::max<std::size_t>(m_blockAlign ? m_blockAlign : 1, static_cast<std::size_t>(bytesPerSec * 0.1));
    if (m_cbuffer.size() > cap) {
        const size_t overflow = m_cbuffer.size() - cap;
        m_cbuffer.erase(m_cbuffer.begin(), m_cbuffer.begin() + overflow);
        KRKR_LOG_WARN("AudioStream: cbuffer overflow trimmed overflow=" + std::to_string(overflow) +
                      " cap=" + std::to_string(cap) +
                      " key=" + std::to_string(key));
    }

    result.cbufferSize = m_cbuffer.size();
    result.appliedSpeed = userSpeed;
    m_lastAppliedSpeed = result.appliedSpeed;
    return result;
}

void AudioStreamProcessor::resetIfIdle(std::chrono::steady_clock::time_point now,
                                       std::chrono::milliseconds idleThreshold, bool shouldLog, std::uintptr_t key) {
    if (m_lastPlayEnd.time_since_epoch().count() == 0) return;
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPlayEnd);
    if (idleMs > idleThreshold) {
        if (!m_cbuffer.empty() && shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: stream reset after idle gap key=" + std::to_string(key) +
                           " idleMs=" + std::to_string(idleMs.count()));
        }
        m_cbuffer.clear();
    }
}

void AudioStreamProcessor::recordPlaybackEnd(float durationSec, float appliedSpeed) {
    const float applied = appliedSpeed > 0.01f ? appliedSpeed : 1.0f;
    const float playTime = durationSec / applied;
    m_lastPlayEnd = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(playTime));
}

} // namespace krkrspeed
