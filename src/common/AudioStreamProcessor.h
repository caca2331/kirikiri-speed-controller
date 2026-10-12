#pragma once

#include <cstdint>
#include <vector>
#include <chrono>
#include <memory>

#include "DspPipeline.h"

namespace krkrspeed {

struct AudioProcessResult {
    std::vector<std::uint8_t> output;
    std::size_t cbufferSize = 0;
    float appliedSpeed = 1.0f;
};

class AudioStreamProcessor {
public:
    AudioStreamProcessor(std::uint32_t sampleRate, std::uint32_t channels, std::uint32_t blockAlign,
                         const DspConfig &cfg, SampleFormat format = SampleFormat::Int16);

    // Both overwrite `result`, reusing its output capacity; callers on the audio path keep one per stream.
    void process(const std::uint8_t *data, std::size_t bytes, float userSpeed, bool shouldLog, std::uintptr_t key,
                 AudioProcessResult &result);
    void processTempoToSize(const std::uint8_t *data, std::size_t inputBytes, std::size_t outputBytes,
                            float userSpeed, bool shouldLog, std::uintptr_t key, AudioProcessResult &result);

    AudioProcessResult process(const std::uint8_t *data, std::size_t bytes, float userSpeed, bool shouldLog,
                               std::uintptr_t key) {
        AudioProcessResult result;
        process(data, bytes, userSpeed, shouldLog, key, result);
        return result;
    }
    AudioProcessResult processTempoToSize(const std::uint8_t *data, std::size_t inputBytes, std::size_t outputBytes,
                                          float userSpeed, bool shouldLog, std::uintptr_t key) {
        AudioProcessResult result;
        processTempoToSize(data, inputBytes, outputBytes, userSpeed, shouldLog, key, result);
        return result;
    }

    void resetIfIdle(std::chrono::steady_clock::time_point now, std::chrono::milliseconds idleThreshold,
                     bool shouldLog, std::uintptr_t key);

    void recordPlaybackEnd(float durationSec, float appliedSpeed);

    float lastAppliedSpeed() const { return m_lastAppliedSpeed; }
    std::chrono::steady_clock::time_point lastPlayEnd() const { return m_lastPlayEnd; }
    std::size_t cbufferSize() const { return m_cbuffer.size() - m_cbufferHead; }

private:
    std::size_t initialPadBytes(std::size_t inputBytes, std::size_t limit) const;
    std::size_t takeFromCbuffer(std::vector<std::uint8_t> &out, std::size_t need);
    void appendToCbuffer(const std::uint8_t *begin, const std::uint8_t *end);
    void clearCbuffer();

    std::uint32_t m_sampleRate = 0;
    std::uint32_t m_blockAlign = 0;
    std::unique_ptr<DspPipeline> m_dsp;
    // Output FIFO (already DSP'd). Bytes before m_cbufferHead are consumed; compacted lazily.
    std::vector<std::uint8_t> m_cbuffer;
    std::size_t m_cbufferHead = 0;
    std::vector<std::uint8_t> m_abuffer;
    std::vector<std::uint8_t> m_dspOut; // DSP output scratch, reused across calls
    std::chrono::steady_clock::time_point m_lastPlayEnd{};
    float m_lastAppliedSpeed = 1.0f;
    bool m_padNext = true;
};

} // namespace krkrspeed
