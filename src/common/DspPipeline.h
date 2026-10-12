#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace krkrspeed {

struct DspConfig {
    float sequenceMs = 35.0f;
    float overlapMs = 10.0f;
    float seekWindowMs = 25.0f;
};

enum class DspMode {
    Tempo,   // change tempo (speed) while keeping pitch
    Pitch    // change pitch while keeping tempo
};

// Interleaved PCM sample layout of the bytes handed to the pipeline.
enum class SampleFormat {
    Int16,
    Int32,
    Float32
};

inline std::uint32_t bytesPerSample(SampleFormat format) {
    return format == SampleFormat::Int16 ? 2u : 4u;
}

class DspPipeline {
public:
    DspPipeline(std::uint32_t sampleRate, std::uint32_t channels, const DspConfig &config = {},
                SampleFormat format = SampleFormat::Int16);
    ~DspPipeline();

    DspPipeline(const DspPipeline &) = delete;
    DspPipeline &operator=(const DspPipeline &) = delete;

    // Replaces `output` with processed PCM bytes in the pipeline's sample format; its capacity is reused.
    // mode=Tempo: adjust tempo by speedRatio (pitch preserved).
    // mode=Pitch: adjust pitch by speedRatio (tempo preserved).
    // Leaves `output` empty when speedRatio is ~1.0 (callers pass the input through).
    void process(const std::uint8_t *data, std::size_t bytes, float speedRatio, DspMode mode,
                 std::vector<std::uint8_t> &output);
    std::vector<std::uint8_t> process(const std::uint8_t *data, std::size_t bytes, float speedRatio,
                                      DspMode mode = DspMode::Tempo) {
        std::vector<std::uint8_t> output;
        process(data, bytes, speedRatio, mode, output);
        return output;
    }

    // Flush internal buffered samples/state.
    void flush();

    std::uint32_t sampleRate() const { return m_sampleRate; }
    std::uint32_t channels() const { return m_channels; }
    SampleFormat format() const { return m_format; }
    const DspConfig &config() const { return m_config; }

private:
    std::uint32_t m_sampleRate;
    std::uint32_t m_channels;
    SampleFormat m_format;
    DspConfig m_config;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace krkrspeed
