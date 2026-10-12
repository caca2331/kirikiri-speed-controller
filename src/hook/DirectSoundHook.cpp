#include "DirectSoundHook.h"
#include "DspRuntime.h"
#include "HookUtils.h"
#include "SharedSettingsManager.h"
#include "SharedStatusManager.h"
#include "../common/Logging.h"
#include "../common/AudioStreamProcessor.h"

#include <initguid.h>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdint>
#include <memory>
#include <cmath>
#include <Psapi.h>

namespace krkrspeed {

namespace {
// Method counts: IDirectSound8 = IUnknown(3) + IDirectSound(8) + VerifyCertification;
// IDirectSoundBuffer8 = IUnknown(3) + IDirectSoundBuffer(18) + SetFX/AcquireResources/GetObjectInPath.
constexpr std::size_t kDeviceVtableSize = 12;
constexpr std::size_t kBufferVtableSize = 24;
constexpr std::size_t kSlotRelease = 2;
constexpr std::size_t kSlotCreateSoundBuffer = 3;
constexpr std::size_t kSlotUnlock = 19;
constexpr long long kBgmReuseWindowMs = 5000;
} // namespace

DirectSoundHook &DirectSoundHook::instance() {
    static DirectSoundHook hook;
    return hook;
}

void DirectSoundHook::installImportHooks() {
    if (m_importHooksInstalled.exchange(true)) {
        return;
    }
    if (PatchImport("dsound.dll", "DirectSoundCreate8", reinterpret_cast<void *>(&DirectSoundHook::DirectSoundCreate8Hook),
                    reinterpret_cast<void **>(&m_origCreate8))) {
        KRKR_LOG_INFO("Patched DirectSoundCreate8 import");
    } else {
        KRKR_LOG_WARN("Failed to patch DirectSoundCreate8 import; will fall back to GetProcAddress interception");
    }
    PatchImport("dsound.dll", "DirectSoundCreate", reinterpret_cast<void *>(&DirectSoundHook::DirectSoundCreateHook),
                reinterpret_cast<void **>(&m_origCreate));
}

void DirectSoundHook::initialize() {
    KRKR_LOG_INFO("DirectSound hook initialization started");
    installImportHooks();
    scanLoadedModules();
    bootstrapVtable();
#if !defined(_WIN64)
    installGlobalUnlockHook();
#else
    KRKR_LOG_INFO("Global Unlock hook skipped on x64 build");
#endif
}

void DirectSoundHook::setOriginalCreate8(void *fn) {
    if (!fn || m_origCreate8) {
        return;
    }
    m_origCreate8 = reinterpret_cast<PFN_DirectSoundCreate8>(fn);
    KRKR_LOG_DEBUG("Captured DirectSoundCreate8 via GetProcAddress; enabling DirectSound interception");
}

void DirectSoundHook::setOriginalCreate(void *fn) {
    if (!fn || m_origCreate) {
        return;
    }
    m_origCreate = reinterpret_cast<PFN_DirectSoundCreate>(fn);
    KRKR_LOG_DEBUG("Captured DirectSoundCreate via GetProcAddress; enabling DirectSound interception");
}

HRESULT WINAPI DirectSoundHook::DirectSoundCreate8Hook(LPCGUID pcGuidDevice, LPDIRECTSOUND8 *ppDS8,
                                                       LPUNKNOWN pUnkOuter) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origCreate8) {
        return DSERR_GENERIC;
    }
    HRESULT hr = hook.m_origCreate8(pcGuidDevice, ppDS8, pUnkOuter);
    if (FAILED(hr) || !ppDS8 || !*ppDS8) {
        return hr;
    }

    hook.patchDeviceVtable(*ppDS8);
    return hr;
}

HRESULT WINAPI DirectSoundHook::DirectSoundCreateHook(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origCreate) {
        return DSERR_GENERIC;
    }
    IDirectSound *ds = nullptr;
    HRESULT hr = hook.m_origCreate(pcGuidDevice, &ds, pUnkOuter);
    if (FAILED(hr) || !ds) {
        return hr;
    }
    // Query for IDirectSound8 to reuse same flow.
    IDirectSound8 *ds8 = nullptr;
    hr = ds->QueryInterface(IID_IDirectSound8, reinterpret_cast<void **>(&ds8));
    ds->Release();
    if (FAILED(hr) || !ds8) {
        return hr;
    }
    hook.patchDeviceVtable(ds8);
    *ppDS = ds8;
    return hr;
}

void DirectSoundHook::scanLoadedModules() {
    HMODULE modules[256];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    const size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(modules));
    for (size_t i = 0; i < count; ++i) {
        char name[MAX_PATH] = {};
        if (GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, static_cast<DWORD>(std::size(name))) == 0) continue;
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("dsound") != std::string::npos) {
            if (!m_origCreate8) {
                if (auto fn = GetProcAddress(modules[i], "DirectSoundCreate8")) {
                    setOriginalCreate8(reinterpret_cast<void *>(fn));
                    KRKR_LOG_INFO(std::string("scanLoadedModules captured DirectSoundCreate8 from ") + name);
                }
            }
            if (!m_origCreate) {
                if (auto fn = GetProcAddress(modules[i], "DirectSoundCreate")) {
                    setOriginalCreate(reinterpret_cast<void *>(fn));
                    KRKR_LOG_INFO(std::string("scanLoadedModules captured DirectSoundCreate from ") + name);
                }
            }
        }
    }
}

void DirectSoundHook::bootstrapVtable() {
    // Patch shared vtable using a temporary DirectSound8 instance.
    if (!m_origCreate8 && !m_origCreate) return;
    IDirectSound8 *ds8 = nullptr;
    if (m_origCreate8) {
        if (FAILED(m_origCreate8(nullptr, &ds8, nullptr)) || !ds8) {
            return;
        }
    } else if (m_origCreate) {
        IDirectSound *ds = nullptr;
        if (FAILED(m_origCreate(nullptr, &ds, nullptr)) || !ds) {
            return;
        }
        ds->QueryInterface(IID_IDirectSound8, reinterpret_cast<void **>(&ds8));
        ds->Release();
        if (!ds8) return;
    }
    ds8->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
    patchDeviceVtable(ds8);
    ds8->Release();
}

DirectSoundHook::BufferPtr DirectSoundHook::makeBufferInfo(const WAVEFORMATEX &fmt, DWORD bufferBytes,
                                                           float bgmGateSeconds) {
    auto info = std::make_shared<BufferInfo>();
    info->sampleRate = fmt.nSamplesPerSec;
    info->channels = fmt.nChannels;
    info->bitsPerSample = fmt.wBitsPerSample;
    info->formatTag = fmt.wFormatTag;
    info->baseFrequency = fmt.nSamplesPerSec;
    info->bufferBytes = bufferBytes;
    info->blockAlign = fmt.nBlockAlign ? fmt.nBlockAlign : (fmt.nChannels * fmt.wBitsPerSample / 8);
    if (info->blockAlign > 0 && fmt.nSamplesPerSec > 0 && bufferBytes > 0) {
        info->approxSeconds = static_cast<float>(bufferBytes) /
                              static_cast<float>(info->blockAlign * fmt.nSamplesPerSec);
    }
    info->isLikelyBgm.store(info->approxSeconds >= bgmGateSeconds);
    info->isPcm16 = (fmt.wFormatTag == WAVE_FORMAT_PCM && fmt.wBitsPerSample == 16);
    if (info->isPcm16 && EnsureDspRuntime()) {
        info->stream = std::make_unique<AudioStreamProcessor>(info->sampleRate, info->channels, info->blockAlign,
                                                              DspConfig{});
    }
    return info;
}

void DirectSoundHook::rememberBgmRelease(std::uintptr_t key, std::chrono::steady_clock::time_point now) {
    for (auto it = m_bgmReleaseTimes.begin(); it != m_bgmReleaseTimes.end();) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
        it = (age > kBgmReuseWindowMs) ? m_bgmReleaseTimes.erase(it) : std::next(it);
    }
    m_bgmReleaseTimes[key] = now;
}

bool DirectSoundHook::consumeRecentBgmRelease(std::uintptr_t key, std::chrono::steady_clock::time_point now) {
    auto it = m_bgmReleaseTimes.find(key);
    if (it == m_bgmReleaseTimes.end()) {
        return false;
    }
    const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
    m_bgmReleaseTimes.erase(it);
    return diff <= kBgmReuseWindowMs;
}

HRESULT WINAPI DirectSoundHook::CreateSoundBufferHook(IDirectSound8 *self, LPDIRECTSOUNDBUFFER *ppDSBuffer,
                                                      LPCDSBUFFERDESC pcDSBufferDesc) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origCreateBuffer) {
        return DSERR_GENERIC;
    }
    HRESULT hr = hook.m_origCreateBuffer(self, ppDSBuffer, pcDSBufferDesc);
    if (FAILED(hr) || !ppDSBuffer || !*ppDSBuffer || !pcDSBufferDesc || !pcDSBufferDesc->lpwfxFormat) {
        return hr;
    }

    const auto *fmt = pcDSBufferDesc->lpwfxFormat;
    std::string fmtKey = "fmt=" + std::to_string(fmt->wFormatTag) + " bits=" + std::to_string(fmt->wBitsPerSample) +
                         " ch=" + std::to_string(fmt->nChannels) + " sr=" + std::to_string(fmt->nSamplesPerSec) +
                         " bytes=" + std::to_string(pcDSBufferDesc->dwBufferBytes) +
                         " flags=" + formatHex(pcDSBufferDesc->dwFlags);
    KRKR_LOG_INFO("DS CreateSoundBuffer " + fmtKey + " buffer=" +
                  std::to_string(reinterpret_cast<std::uintptr_t>(*ppDSBuffer)));
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        if (hook.m_loggedFormats.insert(fmtKey).second) {
            KRKR_LOG_INFO("DS CreateSoundBuffer new format " + fmtKey);
        }
    }

    if ((pcDSBufferDesc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0) {
        KRKR_LOG_INFO("Skip DirectSound Unlock patch on primary buffer");
        return hr;
    }
    if (!(fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16)) {
        KRKR_LOG_WARN("Skip Unlock patch: buffer is not PCM16 (" + fmtKey + ")");
        return hr;
    }

    hook.patchBufferVtable(*ppDSBuffer);

    // Track buffer format.
    const float bgmGate = SharedSettingsManager::instance().snapshot().bgmSecondsGate;
    BufferPtr info = makeBufferInfo(*fmt, pcDSBufferDesc->dwBufferBytes, bgmGate);
    const auto key = reinterpret_cast<std::uintptr_t>(*ppDSBuffer);
    std::lock_guard<std::mutex> lock(hook.m_mutex);
    // If this buffer pointer was recently a BGM buffer and reused quickly, mark again.
    if (hook.consumeRecentBgmRelease(key, std::chrono::steady_clock::now())) {
        info->isLikelyBgm.store(true);
        KRKR_LOG_INFO("DS: buffer reused soon after BGM release; marking BGM buf=" + std::to_string(key));
    }
    hook.m_buffers[key] = std::move(info);
    return hr;
}

HRESULT WINAPI DirectSoundHook::UnlockHook(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                           LPVOID pAudioPtr2, DWORD dwAudioBytes2) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origUnlock) {
        return DSERR_GENERIC;
    }
    if (hook.m_disableAfterFault.load()) {
        return hook.m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }

    // Best-effort safety: if we hit any exception, disable further processing.
    try {
        return hook.handleUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    } catch (...) {
        if (!hook.m_disableAfterFault.exchange(true)) {
            KRKR_LOG_ERROR("DirectSound UnlockHook threw; disabling DS processing for safety");
        }
        hook.m_active.store(false);
        return hook.m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }
}

ULONG __stdcall DirectSoundHook::ReleaseHook(IDirectSoundBuffer *self) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origRelease) {
        return 0;
    }
    const auto key = reinterpret_cast<std::uintptr_t>(self);
    bool wasBgm = false;
    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        auto it = hook.m_buffers.find(key);
        if (it != hook.m_buffers.end()) {
            wasBgm = it->second->isLikelyBgm.load();
        }
    }
    ULONG remaining = hook.m_origRelease(self);
    if (remaining == 0) {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        if (wasBgm) {
            // CreateSoundBuffer/late tracking re-mark a buffer that reappears at this address within 5 s.
            hook.rememberBgmRelease(key, std::chrono::steady_clock::now());
        } else {
            hook.m_bgmReleaseTimes.erase(key);
        }
        hook.m_buffers.erase(key);
        KRKR_LOG_DEBUG("DS buffer released and metadata cleared buf=" + std::to_string(key));
    }
    return remaining;
}

DirectSoundHook::BufferPtr DirectSoundHook::findOrTrackBuffer(IDirectSoundBuffer *self) {
    const auto key = reinterpret_cast<std::uintptr_t>(self);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_buffers.find(key);
        if (it != m_buffers.end()) {
            return it->second;
        }
    }

    // Unknown buffer: discover format and start tracking.
    DWORD cb = 0;
    if (FAILED(self->GetFormat(nullptr, 0, &cb)) || cb < sizeof(WAVEFORMATEX)) {
        KRKR_LOG_WARN("DS Unlock: GetFormat size query failed for untracked buffer; passthrough");
        return nullptr;
    }
    std::vector<std::uint8_t> fmtBuf(cb);
    if (FAILED(self->GetFormat(reinterpret_cast<LPWAVEFORMATEX>(fmtBuf.data()), cb, nullptr))) {
        KRKR_LOG_WARN("DS Unlock: GetFormat failed for untracked buffer; passthrough");
        return nullptr;
    }
    const auto *fx = reinterpret_cast<const WAVEFORMATEX *>(fmtBuf.data());
    // Estimate duration from buffer caps if available.
    DSBCAPS caps{};
    caps.dwSize = sizeof(caps);
    DWORD bytes = 0;
    if (SUCCEEDED(self->GetCaps(&caps))) {
        bytes = caps.dwBufferBytes;
    }
    BufferPtr info = makeBufferInfo(*fx, bytes, SharedSettingsManager::instance().snapshot().bgmSecondsGate);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto inserted = m_buffers.emplace(key, info);
        if (!inserted.second) {
            return inserted.first->second; // another thread tracked it first
        }
        if (consumeRecentBgmRelease(key, std::chrono::steady_clock::now())) {
            info->isLikelyBgm.store(true);
            KRKR_LOG_INFO("DS: buffer reused soon after BGM release; marking BGM buf=" + std::to_string(key));
        }
    }
    KRKR_LOG_INFO("DS Unlock: tracked buffer=" + std::to_string(key) +
                  " fmt=" + std::to_string(fx->wFormatTag) +
                  " bits=" + std::to_string(fx->wBitsPerSample) +
                  " ch=" + std::to_string(fx->nChannels) +
                  " sr=" + std::to_string(fx->nSamplesPerSec));
    if (!info->isPcm16) {
        KRKR_LOG_WARN("DirectSound buffer format not PCM16 (after late track); skipping DSP. fmt=" +
                      std::to_string(info->formatTag) + " bits=" + std::to_string(info->bitsPerSample));
        return info;
    }
    // Patch this buffer's vtable via shadow to ensure future calls are ours.
    patchBufferVtable(self);
    return info;
}

HRESULT DirectSoundHook::handleUnlock(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                      LPVOID pAudioPtr2, DWORD dwAudioBytes2) {
    if (!m_loggedUnlockOnce.exchange(true)) {
        KRKR_LOG_INFO("DirectSound UnlockHook engaged on buffer=" +
                      std::to_string(reinterpret_cast<std::uintptr_t>(self)));
    }
    m_active.store(true);
    SharedStatusManager::instance().setActiveBackend(AudioBackend::DirectSound);
    auto &settingsManager = SharedSettingsManager::instance();
    settingsManager.pollSharedSettings();
    const SharedSettings settings = settingsManager.snapshot();

    // Validate pointers; fall back to passthrough if unsafe.
    auto ptrInvalid = [](LPVOID ptr, DWORD bytes) {
        if (!ptr || bytes == 0) return false;
        return IsBadReadPtr(ptr, bytes) || IsBadWritePtr(ptr, bytes);
    };
    if (ptrInvalid(pAudioPtr1, dwAudioBytes1) || ptrInvalid(pAudioPtr2, dwAudioBytes2)) {
        KRKR_LOG_WARN("DirectSound UnlockHook detected invalid buffer pointers; falling back to passthrough");
        return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }
    const DWORD bytes1 = pAudioPtr1 ? dwAudioBytes1 : 0;
    const DWORD bytes2 = pAudioPtr2 ? dwAudioBytes2 : 0;
    if (bytes1 + bytes2 == 0) {
        KRKR_LOG_DEBUG("DS Unlock: combined buffer empty");
        return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }

    BufferPtr infoPtr = findOrTrackBuffer(self);
    if (!infoPtr || !infoPtr->isPcm16 || !infoPtr->stream) {
        return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }

    {
        BufferInfo &info = *infoPtr;
        std::lock_guard<std::mutex> lock(info.mutex);
        auto &combined = info.combined;
        combined.clear();
        if (bytes1) {
            auto *ptr = reinterpret_cast<std::uint8_t *>(pAudioPtr1);
            combined.insert(combined.end(), ptr, ptr + bytes1);
        }
        if (bytes2) {
            auto *ptr = reinterpret_cast<std::uint8_t *>(pAudioPtr2);
            combined.insert(combined.end(), ptr, ptr + bytes2);
        }

        const auto bufKey = reinterpret_cast<std::uintptr_t>(self);
        const float userSpeed = settings.userSpeed;
        info.unlockCount++;
        if (info.channels == 1) {
            m_seenMono.store(true);
        } else if (info.channels > 1) {
            m_seenStereo.store(true);
        }
        if (!m_loggedMonoStereo.load() && m_seenMono.load() && m_seenStereo.load()) {
            KRKR_LOG_INFO("DS: detected both mono and stereo buffers");
            m_loggedMonoStereo.store(true);
        }

        const std::size_t frames = (combined.size() / sizeof(std::int16_t)) /
                                   std::max<std::uint32_t>(1, info.channels);
        const float durationSec =
            static_cast<float>(frames) / static_cast<float>(std::max<std::uint32_t>(1, info.sampleRate));
        const float totalSec =
            static_cast<float>(info.processedFrames + frames) / static_cast<float>(std::max<std::uint32_t>(1, info.sampleRate));
        const bool shouldLog = info.unlockCount <= 5 || (info.unlockCount % 50 == 0);
        // Reset stream if idle gap exceeded.
        info.stream->resetIfIdle(std::chrono::steady_clock::now(), std::chrono::milliseconds(200), shouldLog, bufKey);

        // stereoBgmMode: 0 aggressive (stereo is BGM), 1 hybrid (stereo is BGM once mono seen), 2 none.
        const bool disableBgm = settings.disableBgm != 0;
        const bool stereoIsBgm = (settings.stereoBgmMode == 0) || (settings.stereoBgmMode == 1 && m_seenMono.load());
        const bool passLengthGate = totalSec > settings.bgmSecondsGate;
        if (!info.isLikelyBgm.load() && !disableBgm && passLengthGate) {
            info.isLikelyBgm.store(true);
            if (shouldLog) {
                KRKR_LOG_INFO("DS buffer marked BGM via length gate buf=" + std::to_string(bufKey) +
                              " totalSec=" + std::to_string(totalSec));
            }
        }
        const bool isBgm = (((info.channels > 1) && stereoIsBgm) || info.isLikelyBgm.load()) && !disableBgm;

        bool doDsp = false;
        if (!isBgm) {
            doDsp = (!settings.lengthGateEnabled || totalSec <= settings.lengthGateSeconds);
        } else if (settings.processAllAudio) {
            doDsp = true;
        }
        if (shouldLog) {
            KRKR_LOG_DEBUG("DS Unlock: buf=" + std::to_string(bufKey) +
                           " bytes=" + std::to_string(combined.size()) +
                           " ch=" + std::to_string(info.channels) +
                           " sr=" + std::to_string(info.sampleRate) +
                           " dur=" + std::to_string(durationSec) +
                           " total=" + std::to_string(totalSec) +
                           " bgm=" + (isBgm ? "1" : "0") +
                           " apply=" + (doDsp ? "1" : "0") +
                           " speed=" + std::to_string(userSpeed));
        }

        const DWORD base = info.baseFrequency ? info.baseFrequency : info.sampleRate;
        float appliedSpeed = 1.0f;
        DWORD desiredFreq = base;
        if (doDsp) {
            // Cap SetFrequency so target Hz never exceeds DSBFREQUENCY_MAX; use applied speed for pitch restore.
            const double scaled = static_cast<double>(base) * static_cast<double>(userSpeed);
            const double clampedD = std::clamp(scaled, static_cast<double>(DSBFREQUENCY_MIN),
                                               static_cast<double>(DSBFREQUENCY_MAX));
            desiredFreq = static_cast<DWORD>(clampedD);
            appliedSpeed = base > 0 ? static_cast<float>(clampedD) / static_cast<float>(base) : userSpeed;
            auto &res = info.processed;
            info.stream->process(combined.data(), combined.size(), appliedSpeed, shouldLog, bufKey, res);
            if (res.output.size() == combined.size()) {
                combined.swap(res.output);
            }
            if (shouldLog) {
                KRKR_LOG_DEBUG("DS SetFrequency applied: base=" + std::to_string(base) +
                               " target=" + std::to_string(desiredFreq) +
                               " appliedSpeed=" + std::to_string(appliedSpeed) +
                               " cbuf=" + std::to_string(res.cbufferSize));
            }
        }

        // Enforce our frequency on buffers we speed up (overrides game resets). Buffers we leave alone keep
        // whatever frequency the game chose; we only restore the base once after we stop processing them.
        if (doDsp || info.frequencyOverridden) {
            DWORD currentFreq = 0;
            bool freqMismatch = true;
            if (SUCCEEDED(self->GetFrequency(&currentFreq))) {
                freqMismatch = (currentFreq != desiredFreq);
            }
            bool ok = true;
            if (freqMismatch) {
                ok = SUCCEEDED(self->SetFrequency(desiredFreq));
                if (!ok) {
                    KRKR_LOG_WARN("DS: SetFrequency failed buf=" + std::to_string(bufKey) +
                                  " desired=" + std::to_string(desiredFreq));
                } else if (shouldLog) {
                    KRKR_LOG_DEBUG("DS: enforced frequency buf=" + std::to_string(bufKey) +
                                   " desired=" + std::to_string(desiredFreq) +
                                   " prev=" + std::to_string(currentFreq));
                }
            }
            info.frequencyOverridden = doDsp || !ok;
        }
        info.processedFrames += frames;

        // Write combined buffer back into the two regions.
        if (bytes1) {
            std::memcpy(pAudioPtr1, combined.data(), bytes1);
        }
        if (bytes2) {
            std::memcpy(pAudioPtr2, combined.data() + bytes1, bytes2);
        }

        // Track expected playback end time for stream reset heuristic.
        info.stream->recordPlaybackEnd(durationSec, doDsp ? appliedSpeed : 1.0f);
    }

    return m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
}

void **DirectSoundHook::shadowFor(void **origVtbl, std::size_t count,
                                  std::unordered_map<void **, std::vector<void *>> &shadows) {
    auto &shadow = shadows[origVtbl];
    if (shadow.empty()) {
        shadow.assign(origVtbl, origVtbl + count);
        m_shadowTables.insert(shadow.data());
    }
    return shadow.data();
}

void DirectSoundHook::patchDeviceVtable(IDirectSound8 *ds8) {
    if (!ds8) return;
    std::lock_guard<std::mutex> lock(m_vtableMutex);
    void **origVtbl = *reinterpret_cast<void ***>(ds8);
    if (!origVtbl || m_shadowTables.count(origVtbl)) {
        return; // already ours
    }
    if (!m_origCreateBuffer) {
        m_origCreateBuffer = reinterpret_cast<PFN_CreateSoundBuffer>(origVtbl[kSlotCreateSoundBuffer]);
    }
    const bool fresh = m_deviceShadows.find(origVtbl) == m_deviceShadows.end();
    void **shadow = shadowFor(origVtbl, kDeviceVtableSize, m_deviceShadows);
    shadow[kSlotCreateSoundBuffer] = reinterpret_cast<void *>(&DirectSoundHook::CreateSoundBufferHook);
    *reinterpret_cast<void ***>(ds8) = shadow;
    if (fresh) {
        KRKR_LOG_INFO("Applied shadow vtable for IDirectSound8 (CreateSoundBuffer)");
    }
}

void DirectSoundHook::patchBufferVtable(IDirectSoundBuffer *buf) {
    if (!buf) return;
    std::lock_guard<std::mutex> lock(m_vtableMutex);
    void **origVtbl = *reinterpret_cast<void ***>(buf);
    if (!origVtbl || m_shadowTables.count(origVtbl)) {
        return; // already ours
    }
    if (!m_origUnlock) {
        m_origUnlock = reinterpret_cast<PFN_Unlock>(origVtbl[kSlotUnlock]);
    }
    if (!m_origRelease) {
        m_origRelease = reinterpret_cast<PFN_Release>(origVtbl[kSlotRelease]);
    }
    const bool fresh = m_bufferShadows.find(origVtbl) == m_bufferShadows.end();
    void **shadow = shadowFor(origVtbl, kBufferVtableSize, m_bufferShadows);
    shadow[kSlotUnlock] = reinterpret_cast<void *>(&DirectSoundHook::UnlockHook);
    shadow[kSlotRelease] = reinterpret_cast<void *>(&DirectSoundHook::ReleaseHook);
    *reinterpret_cast<void ***>(buf) = shadow;
    if (fresh) {
        KRKR_LOG_INFO("Applied shadow vtable for IDirectSoundBuffer (Release, Unlock)");
    }
}

void DirectSoundHook::installGlobalUnlockHook() {
#if defined(_WIN64)
    return;
#else
    // Build a temporary DS buffer to discover the real Unlock implementation.
    if (!m_origCreate8 && !m_origCreate) return;
    IDirectSound8 *ds8 = nullptr;
    if (m_origCreate8) {
        if (FAILED(m_origCreate8(nullptr, &ds8, nullptr)) || !ds8) return;
    } else {
        IDirectSound *ds = nullptr;
        if (FAILED(m_origCreate(nullptr, &ds, nullptr)) || !ds) return;
        ds->QueryInterface(IID_IDirectSound8, reinterpret_cast<void **>(&ds8));
        ds->Release();
        if (!ds8) return;
    }
    ds8->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes = wfx.nAvgBytesPerSec / 2;
    desc.lpwfxFormat = &wfx;
    IDirectSoundBuffer *tmp = nullptr;
    if (FAILED(ds8->CreateSoundBuffer(&desc, &tmp, nullptr)) || !tmp) {
        ds8->Release();
        return;
    }
    void **vtbl = *reinterpret_cast<void ***>(tmp);
    if (vtbl && m_shadowTables.count(vtbl)) {
        vtbl = nullptr; // never detour our own hook
    }
    BYTE *target = vtbl ? reinterpret_cast<BYTE *>(vtbl[kSlotUnlock]) : nullptr;
    tmp->Release();
    ds8->Release();
    if (!target) return;

    static bool installed = false;
    if (installed) return;

    // Only relocate the standard hot-patch prologue (mov edi,edi; push ebp; mov ebp,esp): any other
    // 5-byte prefix may split an instruction or contain relative branches.
    static const BYTE kPrologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
        KRKR_LOG_WARN("Global Unlock detour skipped: unexpected prologue " + formatHex(target[0]) + " " +
                      formatHex(target[1]) + " " + formatHex(target[2]) + " " + formatHex(target[3]) + " " +
                      formatHex(target[4]));
        return;
    }

    // Trampoline: original bytes + jmp back.
    BYTE *trampoline = reinterpret_cast<BYTE *>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return;
    }
    std::memcpy(trampoline, target, 5);
    const intptr_t backRel = (target + 5) - (trampoline + 10);
    trampoline[5] = 0xE9;
    *reinterpret_cast<int32_t *>(trampoline + 6) = static_cast<int32_t>(backRel);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 10);

    // Route UnlockHook through the trampoline before the jump goes live; otherwise a concurrent Unlock
    // could call the patched entry from inside UnlockHook and recurse.
    m_origUnlock = reinterpret_cast<PFN_Unlock>(trampoline);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    const intptr_t rel = reinterpret_cast<BYTE *>(&DirectSoundHook::UnlockHook) - (target + 5);
    BYTE patch[5] = {0xE9};
    *reinterpret_cast<int32_t *>(patch + 1) = static_cast<int32_t>(rel);
    std::memcpy(target, patch, 5);
    VirtualProtect(target, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    installed = true;
    KRKR_LOG_INFO("Installed global Unlock detour via inline jump");
#endif
}

} // namespace krkrspeed
