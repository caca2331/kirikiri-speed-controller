#include "AudioStreamProcessor.h"
#include "Logging.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace krkrspeed {

AudioStreamProcessor::AudioStreamProcessor(std::uint32_t sampleRate, std::uint32_t channels, std::uint32_t blockAlign,
                                           const DspConfig &cfg, SampleFormat format)
    : m_sampleRate(sampleRate), m_blockAlign(blockAlign) {
    m_dsp = std::make_unique<DspPipeline>(sampleRate, channels, cfg, format);
    if (m_blockAlign == 0 && channels > 0) {
        m_blockAlign = channels * bytesPerSample(format);
    }
}

std::size_t AudioStreamProcessor::initialPadBytes(std::size_t inputBytes, std::size_t limit) const {
    const std::size_t bytesPerSec = std::max<std::size_t>(1, static_cast<std::size_t>(m_blockAlign) * m_sampleRate);
    const double durationSec = static_cast<double>(inputBytes) / static_cast<double>(bytesPerSec);
    if (durationSec >= 1.01) {
        return 0;
    }
    const std::size_t align = m_blockAlign ? m_blockAlign : 1;
    std::size_t padBytesTarget = static_cast<std::size_t>(bytesPerSec * 0.03);
    padBytesTarget = (padBytesTarget / align) * align;
    if (padBytesTarget == 0) padBytesTarget = align;
    return std::min(padBytesTarget, limit);
}

std::size_t AudioStreamProcessor::takeFromCbuffer(std::vector<std::uint8_t> &out, std::size_t need) {
    const std::size_t take = std::min(cbufferSize(), need);
    if (take == 0) {
        return 0;
    }
    const auto begin = m_cbuffer.begin() + static_cast<std::ptrdiff_t>(m_cbufferHead);
    out.insert(out.end(), begin, begin + static_cast<std::ptrdiff_t>(take));
    m_cbufferHead += take;
    if (m_cbufferHead == m_cbuffer.size()) {
        clearCbuffer();
    }
    return take;
}

void AudioStreamProcessor::appendToCbuffer(const std::uint8_t *begin, const std::uint8_t *end) {
    if (begin == end) {
        return;
    }
    // Compact only when the consumed prefix dominates, so steady-state appends stay amortized O(1).
    if (m_cbufferHead > 0 && m_cbufferHead >= m_cbuffer.size() / 2) {
        m_cbuffer.erase(m_cbuffer.begin(), m_cbuffer.begin() + static_cast<std::ptrdiff_t>(m_cbufferHead));
        m_cbufferHead = 0;
    }
    m_cbuffer.insert(m_cbuffer.end(), begin, end);
}

void AudioStreamProcessor::clearCbuffer() {
    m_cbuffer.clear();
    m_cbufferHead = 0;
}

void AudioStreamProcessor::process(const std::uint8_t *data, std::size_t bytes, float userSpeed, bool shouldLog,
                                   std::uintptr_t key, AudioProcessResult &result) {
    result.output.clear();
    auto fillPassthrough = [&](float appliedSpeed) {
        if (data && bytes > 0) {
            result.output.assign(data, data + bytes);
        }
        result.cbufferSize = cbufferSize();
        result.appliedSpeed = appliedSpeed;
    };
    if (!data || bytes == 0 || !m_dsp) {
        fillPassthrough(1.0f);
        return;
    }
    if (bytes < 10) {
        // Ignore tiny buffers entirely.
        fillPassthrough(userSpeed);
        return;
    }

    const float denom = std::max(0.01f, userSpeed);
    const float pitchDown = 1.0f / denom;

    result.output.reserve(bytes);
    std::size_t need = bytes;

    if (m_padNext) {
        const std::size_t padBytes = initialPadBytes(bytes, bytes);
        if (padBytes > 0) {
            result.output.insert(result.output.end(), padBytes, 0); // zero padding
            need -= padBytes;
            if (shouldLog) {
                KRKR_LOG_DEBUG("AudioStream: initial front-pad " + std::to_string(padBytes) +
                               " bytes key=" + std::to_string(key));
            }
        }
        m_padNext = false;
    }

    // 1) Consume already-processed tail first; do not re-run through DSP.
    need -= takeFromCbuffer(result.output, need);

    // 2) Always process new input; if output already filled, stash everything to cbuffer.
    auto &out = m_dspOut;
    m_dsp->process(data, bytes, pitchDown, DspMode::Pitch, out);
    if (out.empty()) {
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: pitch-compensate produced 0 bytes; passthrough key=" +
                           std::to_string(key));
        }
        out.assign(data, data + bytes);
    }
    const std::size_t take = std::min<std::size_t>(need, out.size());
    result.output.insert(result.output.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(take));
    need -= take;
    appendToCbuffer(out.data() + take, out.data() + out.size());

    // 3) Front-pad if still short to satisfy exact Abuffer length (per new requirement).
    if (need > 0) {
        result.output.insert(result.output.begin(), need, 0);
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: front-padded " + std::to_string(need) +
                           " bytes (pitch) key=" + std::to_string(key));
        }
        need = 0;
    }

    result.cbufferSize = cbufferSize();
    result.appliedSpeed = userSpeed;
    m_lastAppliedSpeed = result.appliedSpeed;
}

void AudioStreamProcessor::processTempoToSize(const std::uint8_t *data, std::size_t inputBytes,
                                              std::size_t outputBytes, float userSpeed, bool shouldLog,
                                              std::uintptr_t key, AudioProcessResult &result) {
    result.output.clear();
    result.cbufferSize = cbufferSize();
    if (outputBytes == 0) {
        result.appliedSpeed = userSpeed;
        return;
    }

    const float appliedSpeed = userSpeed <= 0.01f ? 1.0f : userSpeed;
    const std::size_t bytesPerSec = std::max<std::size_t>(1, static_cast<std::size_t>(m_blockAlign) * m_sampleRate);
    result.output.reserve(outputBytes);
    std::size_t need = outputBytes;

    if (m_padNext) {
        const std::size_t padBytes = initialPadBytes(inputBytes, need);
        if (padBytes > 0) {
            result.output.insert(result.output.end(), padBytes, 0);
            need -= padBytes;
            if (shouldLog) {
                KRKR_LOG_DEBUG("AudioStream: initial front-pad " + std::to_string(padBytes) +
                               " bytes key=" + std::to_string(key));
            }
        }
        m_padNext = false;
    }

    need -= takeFromCbuffer(result.output, need);

    if (data && inputBytes > 0) {
        m_abuffer.insert(m_abuffer.end(), data, data + inputBytes);
    }

    auto &processed = m_dspOut;
    processed.clear();
    if (m_dsp && !m_abuffer.empty()) {
        const std::size_t align = m_blockAlign ? m_blockAlign : 1;
        std::size_t minBytes = static_cast<std::size_t>(bytesPerSec * 0.03);
        minBytes = (minBytes / align) * align;
        if (minBytes == 0) minBytes = align;
        if (m_abuffer.size() >= minBytes) {
            m_dsp->process(m_abuffer.data(), m_abuffer.size(), appliedSpeed, DspMode::Tempo, processed);
            m_abuffer.clear();
            if (processed.empty() && shouldLog) {
                KRKR_LOG_DEBUG("AudioStream: tempo produced 0 bytes; holding output key=" +
                               std::to_string(key));
            }
        }
    }

    if (!processed.empty()) {
        const std::size_t take = std::min<std::size_t>(need, processed.size());
        result.output.insert(result.output.end(), processed.begin(),
                             processed.begin() + static_cast<std::ptrdiff_t>(take));
        need -= take;
        appendToCbuffer(processed.data() + take, processed.data() + processed.size());
    }

    if (need > 0) {
        result.output.insert(result.output.end(), need, 0);
        if (shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: tail-padded " + std::to_string(need) +
                           " bytes (tempo) key=" + std::to_string(key));
        }
        need = 0;
    }

    result.cbufferSize = cbufferSize();
    result.appliedSpeed = appliedSpeed;
    m_lastAppliedSpeed = result.appliedSpeed;
}

void AudioStreamProcessor::resetIfIdle(std::chrono::steady_clock::time_point now,
                                       std::chrono::milliseconds idleThreshold, bool shouldLog, std::uintptr_t key) {
    if (m_lastPlayEnd.time_since_epoch().count() == 0) return;
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPlayEnd);
    if (idleMs > idleThreshold) {
        if (cbufferSize() > 0 && shouldLog) {
            KRKR_LOG_DEBUG("AudioStream: stream reset after idle gap key=" + std::to_string(key) +
                           " idleMs=" + std::to_string(idleMs.count()));
        }
        clearCbuffer();
        m_abuffer.clear();
        m_padNext = true;
        if (m_dsp) {
            m_dsp->flush();
        }
    }
}

void AudioStreamProcessor::recordPlaybackEnd(float durationSec, float appliedSpeed) {
    const float applied = appliedSpeed > 0.01f ? appliedSpeed : 1.0f;
    const float playTime = durationSec / applied;
    m_lastPlayEnd = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(playTime));
}

} // namespace krkrspeed
