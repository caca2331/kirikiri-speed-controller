#pragma once

#include <Windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <cstdint>
#include <memory>
#include <mutex>

#include "../common/AudioStreamProcessor.h"

namespace krkrspeed::wasapi {

// Per IAudioClient stream state. Fields are guarded by `mutex` once the context is shared.
struct StreamContext {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t formatTag = 0;
    bool isPcm16 = false;
    bool isPcm32 = false;
    bool isFloat32 = false;
    bool formatGuessed = false;    // format came from the default mix format, not IAudioClient::Initialize
    bool channelProbeDone = false; // IAudioStreamVolume channel probe attempted
    bool formatProbeDone = false;  // GetMixFormat probe attempted
    std::unique_ptr<AudioStreamProcessor> stream;
    std::mutex mutex;
    double targetFrames = 0.0;
    std::uint64_t outFrames = 0;
    std::int64_t lastEffective = 0;
};

void parseFormatFlags(const WAVEFORMATEX *format, bool &pcm16, bool &pcm32, bool &float32);
// Remember the first usable format as the default for render clients whose Initialize we missed.
void maybeSetDefaultFormat(const WAVEFORMATEX *format);
std::shared_ptr<StreamContext> createContextFromDefault();
std::shared_ptr<StreamContext> createContextFromFormat(const WAVEFORMATEX *format);

// Creates the DSP stream for the context's current format if missing (and if SoundTouch is available).
void ensureStream(StreamContext &ctx);

// Probes against the live render client; each changes the context only on success.
bool tryResolveChannelCountFromRenderClient(IAudioRenderClient *renderClient, StreamContext &ctx);
bool tryResolveFormatFromRenderClient(IAudioRenderClient *renderClient, StreamContext &ctx);
// Refines a guessed format by inspecting real sample data.
void maybeAdjustGuessedFormat(StreamContext &ctx, const BYTE *buffer, UINT32 frames);

bool isBufferSilent(const StreamContext &ctx, const BYTE *buffer, UINT32 frames);

} // namespace krkrspeed::wasapi
