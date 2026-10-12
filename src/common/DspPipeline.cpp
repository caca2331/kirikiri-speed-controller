#include "DspPipeline.h"
#include "Logging.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <type_traits>

#ifdef USE_SOUNDTOUCH
#include <soundtouch/SoundTouch.h>
#endif

namespace krkrspeed {

namespace {

#ifdef USE_SOUNDTOUCH
using SampleType = soundtouch::SAMPLETYPE;
#else
using SampleType = float;
#endif
constexpr bool kIsFloat = std::is_same_v<SampleType, float>;

float loadSample(const std::uint8_t *p, SampleFormat format) {
    switch (format) {
    case SampleFormat::Int16: {
        std::int16_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) * (1.0f / 32768.0f);
    }
    case SampleFormat::Int32: {
        std::int32_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(static_cast<double>(v) * (1.0 / 2147483648.0));
    }
    case SampleFormat::Float32: {
        float v = 0.0f;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    }
    return 0.0f;
}

void storeSample(std::uint8_t *p, float v, SampleFormat format) {
    switch (format) {
    case SampleFormat::Int16: {
        const float clamped = std::clamp(v, -1.0f, 1.0f);
        const auto s = static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
        std::memcpy(p, &s, sizeof(s));
        return;
    }
    case SampleFormat::Int32: {
        const double clamped = std::clamp(static_cast<double>(v), -1.0, 1.0);
        const auto s = static_cast<std::int32_t>(std::llround(clamped * 2147483647.0));
        std::memcpy(p, &s, sizeof(s));
        return;
    }
    case SampleFormat::Float32:
        std::memcpy(p, &v, sizeof(v));
        return;
    }
}

// Convert interleaved bytes in `format` into SoundTouch samples.
void toSamples(const std::uint8_t *data, std::size_t sampleCount, SampleFormat format, std::vector<SampleType> &out) {
    out.resize(sampleCount);
    const std::uint32_t step = bytesPerSample(format);
    if constexpr (kIsFloat) {
        if (format == SampleFormat::Float32) {
            std::memcpy(out.data(), data, sampleCount * sizeof(float));
            return;
        }
        for (std::size_t i = 0; i < sampleCount; ++i) {
            out[i] = loadSample(data + i * step, format);
        }
    } else {
        if (format == SampleFormat::Int16) {
            std::memcpy(out.data(), data, sampleCount * sizeof(std::int16_t));
            return;
        }
        for (std::size_t i = 0; i < sampleCount; ++i) {
            const float clamped = std::clamp(loadSample(data + i * step, format), -1.0f, 1.0f);
            out[i] = static_cast<SampleType>(std::lround(clamped * 32767.0f));
        }
    }
}

// Append SoundTouch samples to `output` as interleaved bytes in `format`.
void appendSamples(const SampleType *samples, std::size_t sampleCount, SampleFormat format,
                   std::vector<std::uint8_t> &output) {
    const std::uint32_t step = bytesPerSample(format);
    const std::size_t prev = output.size();
    output.resize(prev + sampleCount * step);
    std::uint8_t *dst = output.data() + prev;
    if constexpr (kIsFloat) {
        if (format == SampleFormat::Float32) {
            std::memcpy(dst, samples, sampleCount * sizeof(float));
            return;
        }
        for (std::size_t i = 0; i < sampleCount; ++i) {
            storeSample(dst + i * step, samples[i], format);
        }
    } else {
        if (format == SampleFormat::Int16) {
            std::memcpy(dst, samples, sampleCount * sizeof(std::int16_t));
            return;
        }
        for (std::size_t i = 0; i < sampleCount; ++i) {
            storeSample(dst + i * step, static_cast<float>(samples[i]) * (1.0f / 32768.0f), format);
        }
    }
}

} // namespace

struct DspPipeline::Impl {
#ifdef USE_SOUNDTOUCH
    soundtouch::SoundTouch touch;
#endif
    std::vector<SampleType> input;
    std::vector<SampleType> scratch;
    std::mutex mutex;
};

DspPipeline::DspPipeline(std::uint32_t sampleRate, std::uint32_t channels, const DspConfig &config,
                         SampleFormat format)
    : m_sampleRate(sampleRate), m_channels(channels), m_format(format), m_config(config),
      m_impl(std::make_unique<Impl>()) {
#ifdef USE_SOUNDTOUCH
    m_impl->touch.setSampleRate(sampleRate);
    m_impl->touch.setChannels(static_cast<unsigned int>(channels));
    m_impl->touch.setSetting(SETTING_SEQUENCE_MS, static_cast<int>(config.sequenceMs));
    m_impl->touch.setSetting(SETTING_OVERLAP_MS, static_cast<int>(config.overlapMs));
    m_impl->touch.setSetting(SETTING_SEEKWINDOW_MS, static_cast<int>(config.seekWindowMs));
#endif
}

DspPipeline::~DspPipeline() = default;

std::vector<std::uint8_t> DspPipeline::process(const std::uint8_t *data, std::size_t bytes, float speedRatio,
                                              DspMode mode) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!data || bytes == 0 || m_channels == 0) {
        return {};
    }

    // Near-1.0 speed: bypass DSP entirely and let callers passthrough the original buffer.
    if (std::fabs(speedRatio - 1.0f) <= 0.001f) {
        return {};
    }

    const float ratio = std::max(0.01f, speedRatio);
    const std::size_t sampleCount = bytes / bytesPerSample(m_format);
    const std::size_t frameCount = sampleCount / m_channels;
    if (frameCount == 0) {
        return {};
    }

    toSamples(data, frameCount * m_channels, m_format, m_impl->input);
    std::vector<std::uint8_t> output;

#ifdef USE_SOUNDTOUCH
    if (mode == DspMode::Tempo) {
        m_impl->touch.setTempo(ratio);
        m_impl->touch.setRate(1.0f);
        m_impl->touch.setPitch(1.0f);
    } else {
        m_impl->touch.setTempo(1.0f);
        m_impl->touch.setRate(1.0f);
        m_impl->touch.setPitch(ratio);
    }

    m_impl->touch.putSamples(m_impl->input.data(), static_cast<unsigned int>(frameCount));

    const float outScale = (mode == DspMode::Tempo) ? std::max(0.1f, ratio) : 1.0f;
    const std::size_t maxFrames = static_cast<std::size_t>(std::ceil(frameCount / outScale)) + 1024;
    m_impl->scratch.resize(maxFrames * m_channels);
    output.reserve(maxFrames * m_channels * bytesPerSample(m_format));

    while (true) {
        const auto receivedFrames =
            m_impl->touch.receiveSamples(m_impl->scratch.data(), static_cast<unsigned int>(maxFrames));
        if (receivedFrames == 0) break;
        appendSamples(m_impl->scratch.data(), receivedFrames * m_channels, m_format, output);
        if (output.size() >= bytes && mode == DspMode::Tempo) break;
    }
#else
    // Fallback without SoundTouch: tempo = linear resample (pitch shifts), pitch = passthrough.
    if (mode == DspMode::Tempo) {
        const std::size_t outFrames = static_cast<std::size_t>(static_cast<double>(frameCount) / ratio);
        m_impl->scratch.resize(outFrames * m_channels);
        for (std::size_t i = 0; i < outFrames; ++i) {
            const double src = static_cast<double>(i) * ratio;
            const std::size_t idx = std::min(static_cast<std::size_t>(src), frameCount - 1);
            const std::size_t next = std::min(idx + 1, frameCount - 1);
            const float frac = static_cast<float>(src - static_cast<double>(idx));
            for (std::size_t c = 0; c < m_channels; ++c) {
                const float a = m_impl->input[idx * m_channels + c];
                const float b = m_impl->input[next * m_channels + c];
                m_impl->scratch[i * m_channels + c] = a + (b - a) * frac;
            }
        }
        appendSamples(m_impl->scratch.data(), m_impl->scratch.size(), m_format, output);
    }
#endif

    if (output.empty() && mode == DspMode::Pitch) {
        return std::vector<std::uint8_t>(data, data + bytes);
    }
    return output;
}

void DspPipeline::flush() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
#ifdef USE_SOUNDTOUCH
    m_impl->touch.clear();
#endif
}

} // namespace krkrspeed
