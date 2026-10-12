// DSP smoke tests: platform independent; with SoundTouch disabled they exercise the fallback DSP.
#include "common/AudioStreamProcessor.h"
#include "common/DspPipeline.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace krkrspeed;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
        }                                                                              \
    } while (0)

std::vector<std::uint8_t> makeInt16Ramp(std::size_t samples, std::int16_t start) {
    std::vector<std::uint8_t> bytes(samples * sizeof(std::int16_t));
    for (std::size_t i = 0; i < samples; ++i) {
        const auto v = static_cast<std::int16_t>(start + static_cast<std::int16_t>(i % 2000));
        std::memcpy(bytes.data() + i * 2, &v, 2);
    }
    return bytes;
}

std::vector<std::uint8_t> makeSine(SampleFormat format, std::size_t frames, std::uint32_t channels) {
    const std::uint32_t step = bytesPerSample(format);
    std::vector<std::uint8_t> bytes(frames * channels * step);
    for (std::size_t f = 0; f < frames; ++f) {
        const float v = 0.5f * std::sin(static_cast<float>(f) * 0.05f);
        for (std::uint32_t c = 0; c < channels; ++c) {
            std::uint8_t *p = bytes.data() + (f * channels + c) * step;
            if (format == SampleFormat::Int16) {
                const auto s = static_cast<std::int16_t>(std::lround(v * 32767.0f));
                std::memcpy(p, &s, 2);
            } else if (format == SampleFormat::Int32) {
                const auto s = static_cast<std::int32_t>(std::llround(v * 2147483647.0));
                std::memcpy(p, &s, 4);
            } else {
                std::memcpy(p, &v, 4);
            }
        }
    }
    return bytes;
}

// At 1.0x the pitch path is a pure delay line: output = 30 ms of zeros followed by the input stream.
void testPitchPassthroughIsContinuous() {
    const std::uint32_t rate = 8000;
    AudioStreamProcessor proc(rate, 1, 2, DspConfig{});
    std::vector<std::uint8_t> inputStream;
    std::vector<std::uint8_t> outputStream;
    const std::size_t chunkSizes[] = {800, 1200, 64, 4000, 802, 2000};
    std::int16_t start = 0;
    for (int round = 0; round < 5; ++round) {
        for (std::size_t samples : chunkSizes) {
            auto chunk = makeInt16Ramp(samples, start);
            start = static_cast<std::int16_t>(start + 7);
            auto res = proc.process(chunk.data(), chunk.size(), 1.0f, false, 1);
            CHECK(res.output.size() == chunk.size());
            inputStream.insert(inputStream.end(), chunk.begin(), chunk.end());
            outputStream.insert(outputStream.end(), res.output.begin(), res.output.end());
        }
    }
    const std::size_t padBytes = static_cast<std::size_t>(rate * 2 * 0.03);
    CHECK(outputStream.size() == inputStream.size());
    bool zeros = true;
    for (std::size_t i = 0; i < padBytes; ++i) zeros &= (outputStream[i] == 0);
    CHECK(zeros);
    CHECK(std::memcmp(outputStream.data() + padBytes, inputStream.data(), inputStream.size() - padBytes) == 0);
    CHECK(proc.cbufferSize() == padBytes);
}

void testPitchKeepsExactSize() {
    for (SampleFormat format : {SampleFormat::Int16, SampleFormat::Int32, SampleFormat::Float32}) {
        const std::uint32_t channels = 2;
        const std::uint32_t blockAlign = channels * bytesPerSample(format);
        AudioStreamProcessor proc(44100, channels, blockAlign, DspConfig{}, format);
        for (float speed : {1.5f, 0.75f, 2.0f}) {
            for (std::size_t frames : {441u, 2205u, 100u}) {
                auto chunk = makeSine(format, frames, channels);
                auto res = proc.process(chunk.data(), chunk.size(), speed, false, 2);
                CHECK(res.output.size() == chunk.size());
            }
        }
    }
}

void testTempoToSizeHonorsOutputSize() {
    for (SampleFormat format : {SampleFormat::Int16, SampleFormat::Int32, SampleFormat::Float32}) {
        const std::uint32_t channels = 2;
        const std::uint32_t blockAlign = channels * bytesPerSample(format);
        AudioStreamProcessor proc(48000, channels, blockAlign, DspConfig{}, format);
        for (float speed : {1.5f, 2.0f, 3.0f}) {
            for (int i = 0; i < 50; ++i) {
                const std::size_t inFrames = 480;
                const std::size_t outFrames = static_cast<std::size_t>(inFrames / speed);
                auto chunk = makeSine(format, inFrames, channels);
                auto res = proc.processTempoToSize(chunk.data(), chunk.size(), outFrames * blockAlign, speed, false, 3);
                CHECK(res.output.size() == outFrames * blockAlign);
            }
        }
    }
}

void testIdleResetClearsState() {
    AudioStreamProcessor proc(8000, 1, 2, DspConfig{});
    auto chunk = makeInt16Ramp(800, 100);
    proc.process(chunk.data(), chunk.size(), 1.0f, false, 4);
    CHECK(proc.cbufferSize() > 0);
    proc.recordPlaybackEnd(0.1f, 1.0f);
    proc.resetIfIdle(std::chrono::steady_clock::now() + std::chrono::seconds(1), std::chrono::milliseconds(200), false,
                     4);
    CHECK(proc.cbufferSize() == 0);
    // A fresh stream starts with the 30 ms front pad again.
    auto res = proc.process(chunk.data(), chunk.size(), 1.0f, false, 4);
    CHECK(res.output.size() == chunk.size());
    CHECK(res.output[0] == 0 && res.output[1] == 0);
}

void testDspFormatsRoundTrip() {
    const std::uint32_t channels = 2;
    for (SampleFormat format : {SampleFormat::Int16, SampleFormat::Int32, SampleFormat::Float32}) {
        DspPipeline dsp(44100, channels, DspConfig{}, format);
        auto input = makeSine(format, 4410, channels);
        CHECK(dsp.process(input.data(), input.size(), 1.0f, DspMode::Tempo).empty());
        auto out = dsp.process(input.data(), input.size(), 2.0f, DspMode::Tempo);
        const std::uint32_t blockAlign = channels * bytesPerSample(format);
        CHECK(out.size() % blockAlign == 0);
#ifndef USE_SOUNDTOUCH
        // Fallback resampler at 2x picks every second frame exactly.
        CHECK(out.size() == input.size() / 2);
        bool close = true;
        for (std::size_t f = 0; f < out.size() / blockAlign; ++f) {
            const std::size_t outOff = f * blockAlign;
            const std::size_t inOff = (f * 2) * blockAlign;
            if (format == SampleFormat::Float32) {
                close &= std::memcmp(out.data() + outOff, input.data() + inOff, 4) == 0;
            } else if (format == SampleFormat::Int16) {
                std::int16_t a = 0, b = 0;
                std::memcpy(&a, out.data() + outOff, 2);
                std::memcpy(&b, input.data() + inOff, 2);
                close &= std::abs(a - b) <= 2;
            } else {
                std::int32_t a = 0, b = 0;
                std::memcpy(&a, out.data() + outOff, 4);
                std::memcpy(&b, input.data() + inOff, 4);
                close &= std::llabs(static_cast<long long>(a) - b) <= 512;
            }
        }
        CHECK(close);
#endif
    }
}

} // namespace

int main() {
    testPitchPassthroughIsContinuous();
    testPitchKeepsExactSize();
    testTempoToSizeHonorsOutputSize();
    testIdleResetClearsState();
    testDspFormatsRoundTrip();
    if (g_failures == 0) {
        std::cout << "All DSP tests passed\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
}
