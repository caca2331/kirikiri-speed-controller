#include "WasapiFormat.h"
#include "DspRuntime.h"
#include "../common/Logging.h"

#include <ksmedia.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace krkrspeed::wasapi {

namespace {

struct DefaultFormat {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t formatTag = 0;
    bool isPcm16 = false;
    bool isPcm32 = false;
    bool isFloat32 = false;
};

std::mutex g_formatMutex;
DefaultFormat g_defaultFormat;
bool g_haveDefaultFormat = false;
std::atomic<bool> g_loggedDefaultFormat{false};

std::uint32_t sampleBytes(const StreamContext &ctx) {
    return (ctx.isFloat32 || ctx.isPcm32) ? 4u : 2u;
}

std::string describeFormat(const WAVEFORMATEX *format, bool pcm16, bool pcm32, bool float32) {
    std::string msg = " tag=" + std::to_string(format->wFormatTag) +
                      " ch=" + std::to_string(format->nChannels) +
                      " rate=" + std::to_string(format->nSamplesPerSec) +
                      " bits=" + std::to_string(format->wBitsPerSample);
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            msg += " sub=PCM";
        } else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            msg += " sub=FLOAT";
        } else {
            msg += " sub=" + std::to_string(ext->SubFormat.Data1);
        }
    }
    if (pcm16) {
        msg += " fmt=pcm16";
    } else if (pcm32) {
        msg += " fmt=pcm32";
    } else if (float32) {
        msg += " fmt=float32";
    }
    return msg;
}

// Switch the context to a newly detected sample format; the DSP stream is rebuilt for it.
void finalizeContextFormat(StreamContext &ctx, bool pcm16, bool pcm32, bool float32) {
    ctx.isPcm16 = pcm16;
    ctx.isPcm32 = pcm32;
    ctx.isFloat32 = float32;
    ctx.formatGuessed = false;
    if (ctx.channels > 0) {
        ctx.blockAlign = ctx.channels * sampleBytes(ctx);
    }
    ctx.stream.reset();
    ensureStream(ctx);
}

bool applyFormatToContext(StreamContext &ctx, const WAVEFORMATEX *format) {
    if (!format) return false;
    ctx.sampleRate = format->nSamplesPerSec;
    ctx.channels = format->nChannels;
    ctx.bitsPerSample = format->wBitsPerSample;
    ctx.formatTag = format->wFormatTag;
    bool pcm16 = false;
    bool pcm32 = false;
    bool float32 = false;
    parseFormatFlags(format, pcm16, pcm32, float32);
    finalizeContextFormat(ctx, pcm16, pcm32, float32);
    return (pcm16 || pcm32 || float32);
}

bool looksLikePcm16(const BYTE *buffer, std::size_t bytes) {
    if (!buffer || bytes < sizeof(std::int16_t) * 8) {
        return false;
    }
    const std::size_t count = bytes / sizeof(std::int16_t);
    const auto *samples = reinterpret_cast<const std::int16_t *>(buffer);
    std::size_t smooth = 0;
    std::int64_t sumAbs = 0;
    for (std::size_t i = 1; i < count; ++i) {
        const std::int32_t cur = samples[i];
        const std::int32_t prev = samples[i - 1];
        sumAbs += std::abs(cur);
        if (std::abs(cur - prev) < 2000) {
            smooth++;
        }
    }
    const double avgAbs = static_cast<double>(sumAbs) / static_cast<double>(count);
    const double smoothPct = (count > 1) ? (static_cast<double>(smooth) * 100.0 / static_cast<double>(count - 1)) : 0.0;
    return (avgAbs > 100.0 && smoothPct > 12.0);
}

} // namespace

void parseFormatFlags(const WAVEFORMATEX *format, bool &pcm16, bool &pcm32, bool &float32) {
    pcm16 = false;
    pcm32 = false;
    float32 = false;
    if (!format) return;
    pcm16 = (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16);
    pcm32 = (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 32);
    float32 = (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32);
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && format->wBitsPerSample == 16) {
            pcm16 = true;
        }
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && format->wBitsPerSample == 32) {
            pcm32 = true;
        }
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && format->wBitsPerSample == 32) {
            float32 = true;
        }
    }
}

void maybeSetDefaultFormat(const WAVEFORMATEX *format) {
    if (!format) return;
    DefaultFormat fmt{};
    fmt.sampleRate = format->nSamplesPerSec;
    fmt.channels = format->nChannels;
    fmt.blockAlign = format->nBlockAlign ? format->nBlockAlign : (format->nChannels * format->wBitsPerSample / 8);
    fmt.bitsPerSample = format->wBitsPerSample;
    fmt.formatTag = format->wFormatTag;
    parseFormatFlags(format, fmt.isPcm16, fmt.isPcm32, fmt.isFloat32);
    if (fmt.sampleRate == 0 || fmt.channels == 0 || (!fmt.isPcm16 && !fmt.isPcm32 && !fmt.isFloat32)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_formatMutex);
    if (g_haveDefaultFormat) return;
    g_defaultFormat = fmt;
    g_haveDefaultFormat = true;
    if (!g_loggedDefaultFormat.exchange(true)) {
        KRKR_LOG_INFO("WASAPI default mix format captured" +
                      describeFormat(format, fmt.isPcm16, fmt.isPcm32, fmt.isFloat32));
    }
}

std::shared_ptr<StreamContext> createContextFromDefault() {
    DefaultFormat fmt;
    {
        std::lock_guard<std::mutex> lock(g_formatMutex);
        if (!g_haveDefaultFormat) {
            return nullptr;
        }
        fmt = g_defaultFormat;
    }
    auto ctx = std::make_shared<StreamContext>();
    ctx->sampleRate = fmt.sampleRate;
    ctx->channels = fmt.channels;
    ctx->blockAlign = fmt.blockAlign;
    ctx->bitsPerSample = fmt.bitsPerSample;
    ctx->formatTag = fmt.formatTag;
    ctx->isPcm16 = fmt.isPcm16;
    ctx->isPcm32 = fmt.isPcm32;
    ctx->isFloat32 = fmt.isFloat32;
    ctx->formatGuessed = true;
    ensureStream(*ctx);
    return ctx;
}

std::shared_ptr<StreamContext> createContextFromFormat(const WAVEFORMATEX *format) {
    auto ctx = std::make_shared<StreamContext>();
    ctx->sampleRate = format->nSamplesPerSec;
    ctx->channels = format->nChannels;
    ctx->blockAlign = format->nBlockAlign ? format->nBlockAlign : (format->nChannels * format->wBitsPerSample / 8);
    ctx->bitsPerSample = format->wBitsPerSample;
    ctx->formatTag = format->wFormatTag;
    parseFormatFlags(format, ctx->isPcm16, ctx->isPcm32, ctx->isFloat32);
    ctx->formatGuessed = false;
    ensureStream(*ctx);
    return ctx;
}

void ensureStream(StreamContext &ctx) {
    if (ctx.stream || ctx.sampleRate == 0 || ctx.channels == 0 || ctx.blockAlign == 0) {
        return;
    }
    if (!ctx.isPcm16 && !ctx.isPcm32 && !ctx.isFloat32) {
        return;
    }
    // The DSP treats each frame as `channels` packed samples; anything else is passed through.
    if (ctx.blockAlign != ctx.channels * sampleBytes(ctx)) {
        return;
    }
    if (!EnsureDspRuntime()) {
        return;
    }
    const SampleFormat format = ctx.isFloat32 ? SampleFormat::Float32
                                : (ctx.isPcm32 ? SampleFormat::Int32 : SampleFormat::Int16);
    ctx.stream = std::make_unique<AudioStreamProcessor>(ctx.sampleRate, ctx.channels, ctx.blockAlign, DspConfig{},
                                                        format);
}

bool tryResolveChannelCountFromRenderClient(IAudioRenderClient *renderClient, StreamContext &ctx) {
    if (!renderClient) {
        return false;
    }
    IAudioClient *audioClient = nullptr;
    HRESULT hr = renderClient->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&audioClient));
    if (FAILED(hr) || !audioClient) {
        return false;
    }
    IAudioStreamVolume *volume = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioStreamVolume), reinterpret_cast<void **>(&volume));
    if (FAILED(hr) || !volume) {
        audioClient->Release();
        return false;
    }
    UINT32 channels = 0;
    hr = volume->GetChannelCount(&channels);
    volume->Release();
    audioClient->Release();
    if (FAILED(hr) || channels == 0) {
        return false;
    }
    if (ctx.channels != channels) {
        ctx.channels = channels;
        ctx.blockAlign = ctx.channels * sampleBytes(ctx);
        ctx.stream.reset();
        ctx.formatGuessed = true;
        ensureStream(ctx);
        KRKR_LOG_INFO("WASAPI resolved channel count via IAudioStreamVolume: ch=" + std::to_string(channels));
    }
    return true;
}

bool tryResolveFormatFromRenderClient(IAudioRenderClient *renderClient, StreamContext &ctx) {
    if (!renderClient || !ctx.formatGuessed) {
        return false;
    }
    IAudioClient *audioClient = nullptr;
    HRESULT hr = renderClient->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&audioClient));
    if (FAILED(hr) || !audioClient) {
        return false;
    }
    WAVEFORMATEX *mix = nullptr;
    hr = audioClient->GetMixFormat(&mix);
    bool ok = false;
    if (SUCCEEDED(hr) && mix) {
        ok = applyFormatToContext(ctx, mix);
        CoTaskMemFree(mix);
    }
    audioClient->Release();
    if (ok) {
        KRKR_LOG_INFO("WASAPI resolved stream format via render client");
    }
    return ok;
}

bool isBufferSilent(const StreamContext &ctx, const BYTE *buffer, UINT32 frames) {
    if (!buffer || frames == 0 || ctx.channels == 0) {
        return true;
    }
    const std::size_t maxSamples = std::min<std::size_t>(static_cast<std::size_t>(frames) * ctx.channels, 256);
    if (maxSamples == 0) {
        return true;
    }
    if (ctx.isFloat32) {
        const float *samples = reinterpret_cast<const float *>(buffer);
        for (std::size_t i = 0; i < maxSamples; ++i) {
            const float v = samples[i];
            if (!std::isfinite(v) || std::fabs(v) >= 1e-4f) {
                return false;
            }
        }
        return true;
    }
    if (ctx.isPcm32) {
        const std::int32_t *samples = reinterpret_cast<const std::int32_t *>(buffer);
        for (std::size_t i = 0; i < maxSamples; ++i) {
            const std::int32_t v = samples[i];
            const std::int32_t av = (v == std::numeric_limits<std::int32_t>::min())
                ? std::numeric_limits<std::int32_t>::max()
                : std::abs(v);
            if (av >= (32 << 16)) {
                return false;
            }
        }
        return true;
    }
    const std::int16_t *samples = reinterpret_cast<const std::int16_t *>(buffer);
    for (std::size_t i = 0; i < maxSamples; ++i) {
        const std::int16_t v = samples[i];
        const int av = (v == std::numeric_limits<std::int16_t>::min()) ? std::numeric_limits<std::int16_t>::max()
                                                                       : std::abs(static_cast<int>(v));
        if (av >= 8) {
            return false;
        }
    }
    return true;
}

void maybeAdjustGuessedFormat(StreamContext &ctx, const BYTE *buffer, UINT32 frames) {
    if (!ctx.formatGuessed || !buffer || frames == 0) {
        return;
    }
    const std::size_t maxBytes = std::min<std::size_t>(512, static_cast<std::size_t>(frames) * ctx.channels * 4);
    if (maxBytes < 8) {
        return;
    }
    const std::size_t wordBytes = maxBytes & ~static_cast<std::size_t>(3);
    const std::size_t wordCount = wordBytes / sizeof(std::uint32_t);
    if (wordCount == 0) {
        return;
    }
    std::size_t normalCount = 0;
    std::size_t denormCount = 0;
    std::size_t nanInfCount = 0;
    std::size_t finiteCount = 0;
    std::size_t within2Count = 0;
    double maxAbs = 0.0;
    double sumAbs = 0.0;
    for (std::size_t i = 0; i < wordCount; ++i) {
        std::uint32_t u = 0;
        std::memcpy(&u, buffer + i * sizeof(u), sizeof(u));
        const std::uint32_t exp = (u >> 23) & 0xFFu;
        if (exp == 0) {
            denormCount++;
        } else if (exp == 0xFFu) {
            nanInfCount++;
        } else {
            normalCount++;
        }
        float v = 0.0f;
        std::memcpy(&v, &u, sizeof(v));
        if (std::isfinite(v)) {
            const double av = std::fabs(static_cast<double>(v));
            sumAbs += av;
            finiteCount++;
            if (av <= 2.0) {
                within2Count++;
            }
            if (av > maxAbs) {
                maxAbs = av;
            }
        }
    }
    const std::size_t total = normalCount + denormCount + nanInfCount;
    if (total == 0 || finiteCount == 0) {
        return;
    }
    const double avgAbs = sumAbs / static_cast<double>(finiteCount);
    const double within2Pct = static_cast<double>(within2Count) * 100.0 / static_cast<double>(finiteCount);

    // If buffer is essentially silent, defer decision until we see real audio.
    if (avgAbs < 1e-5 && maxAbs < 1e-4) {
        return;
    }

    // Prefer float if values look like normalized audio.
    if (within2Pct >= 90.0 && nanInfCount * 100 < total * 1 && maxAbs <= 2.5) {
        finalizeContextFormat(ctx, false, false, true);
        KRKR_LOG_INFO("WASAPI guessed stream format: float32");
        return;
    }
    // If float interpretation is clearly invalid, fall back to PCM formats.
    if (maxAbs > 8.0 || nanInfCount * 100 >= total * 5) {
        if (looksLikePcm16(buffer, wordBytes)) {
            finalizeContextFormat(ctx, true, false, false);
            KRKR_LOG_INFO("WASAPI guessed stream format: pcm16");
        }
        // Otherwise keep the current guess until PCM16 can be validated.
    }
    // If unsure, keep current guessed format (likely float32 from mix).
}

} // namespace krkrspeed::wasapi
