#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace krkrspeed {

// Frame accounting for the WASAPI speed-up route, which drops frames at ReleaseBuffer: each period
// submits about framesWritten / speed frames, and the running total stays within 30 ms of the ideal.
struct FrameDropBudget {
    double targetFrames = 0.0;   // ideal number of frames submitted so far
    std::uint64_t outFrames = 0; // frames actually submitted so far (see commit)

    void reset() {
        targetFrames = 0.0;
        outFrames = 0;
    }

    // Frames to submit for a period of `framesWritten` frames, in [1, framesWritten] (0 for an empty period).
    std::uint32_t plan(std::uint32_t framesWritten, float speed, std::uint32_t sampleRate) {
        if (framesWritten == 0) {
            return 0;
        }
        targetFrames += static_cast<double>(framesWritten) / static_cast<double>(std::max(0.01f, speed));
        const std::int64_t desiredTotal = static_cast<std::int64_t>(std::llround(targetFrames));
        std::int64_t eff = desiredTotal - static_cast<std::int64_t>(outFrames);

        if (sampleRate > 0) {
            const std::int64_t maxDrift = static_cast<std::int64_t>(std::llround(static_cast<double>(sampleRate) * 0.03));
            if (maxDrift > 0) {
                const double drift = static_cast<double>(maxDrift);
                const double out = static_cast<double>(outFrames);
                const auto minEff = static_cast<std::int64_t>(std::floor(targetFrames - drift - out));
                const auto maxEff = static_cast<std::int64_t>(std::ceil(targetFrames + drift - out));
                eff = std::clamp(eff, minEff, maxEff);
            }
        }

        eff = std::clamp<std::int64_t>(eff, 1, static_cast<std::int64_t>(framesWritten));
        return static_cast<std::uint32_t>(eff);
    }

    // Records the frames really handed to the endpoint for this period.
    void commit(std::uint32_t frames) { outFrames += frames; }
};

} // namespace krkrspeed
