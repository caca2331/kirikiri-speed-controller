#include "DirectSoundHook.h"
#include "HookUtils.h"
#include "XAudio2Hook.h"
#include "../common/Logging.h"

#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdint>
#include <memory>

namespace krkrspeed {

DirectSoundHook &DirectSoundHook::instance() {
    static DirectSoundHook hook;
    return hook;
}

void DirectSoundHook::initialize() {
    KRKR_LOG_INFO("DirectSound hook initialization started");
    hookEntryPoints();
}

void DirectSoundHook::setOriginalCreate8(void *fn) {
    if (!fn || m_origCreate8) {
        return;
    }
    m_origCreate8 = reinterpret_cast<PFN_DirectSoundCreate8>(fn);
    KRKR_LOG_DEBUG("Captured DirectSoundCreate8 via GetProcAddress; enabling DirectSound interception");
}

void DirectSoundHook::hookEntryPoints() {
    if (PatchImport("dsound.dll", "DirectSoundCreate8", reinterpret_cast<void *>(&DirectSoundHook::DirectSoundCreate8Hook),
                    reinterpret_cast<void **>(&m_origCreate8))) {
        KRKR_LOG_INFO("Patched DirectSoundCreate8 import");
    } else {
        KRKR_LOG_WARN("Failed to patch DirectSoundCreate8 import; will fall back to GetProcAddress interception");
    }
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

    void **vtbl = *reinterpret_cast<void ***>(*ppDS8);
    if (!hook.m_origCreateBuffer) {
        PatchVtableEntry(vtbl, 3, &DirectSoundHook::CreateSoundBufferHook, hook.m_origCreateBuffer);
        KRKR_LOG_INFO("Patched IDirectSound8 vtable (CreateSoundBuffer)");
    }
    return hr;
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

    void **vtbl = *reinterpret_cast<void ***>(*ppDSBuffer);
    if (!hook.m_origUnlock) {
        PatchVtableEntry(vtbl, 19, &DirectSoundHook::UnlockHook, hook.m_origUnlock);
        KRKR_LOG_INFO("Patched IDirectSoundBuffer vtable (Unlock)");
    }

    // Track buffer format.
    std::lock_guard<std::mutex> lock(hook.m_mutex);
    BufferInfo info;
    info.sampleRate = pcDSBufferDesc->lpwfxFormat->nSamplesPerSec;
    info.channels = pcDSBufferDesc->lpwfxFormat->nChannels;
    DspConfig cfg{};
    info.dsp = std::make_unique<DspPipeline>(info.sampleRate, info.channels, cfg);
    hook.m_buffers[reinterpret_cast<std::uintptr_t>(*ppDSBuffer)] = std::move(info);

    return hr;
}

HRESULT WINAPI DirectSoundHook::UnlockHook(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                           LPVOID pAudioPtr2, DWORD dwAudioBytes2) {
    auto &hook = DirectSoundHook::instance();
    if (!hook.m_origUnlock) {
        return DSERR_GENERIC;
    }

    std::vector<std::uint8_t> combined;
    combined.reserve(dwAudioBytes1 + dwAudioBytes2);
    if (pAudioPtr1 && dwAudioBytes1) {
        auto *ptr = reinterpret_cast<std::uint8_t *>(pAudioPtr1);
        combined.insert(combined.end(), ptr, ptr + dwAudioBytes1);
    }
    if (pAudioPtr2 && dwAudioBytes2) {
        auto *ptr = reinterpret_cast<std::uint8_t *>(pAudioPtr2);
        combined.insert(combined.end(), ptr, ptr + dwAudioBytes2);
    }

    if (combined.empty()) {
        return hook.m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
    }

    {
        std::lock_guard<std::mutex> lock(hook.m_mutex);
        auto it = hook.m_buffers.find(reinterpret_cast<std::uintptr_t>(self));
        if (it != hook.m_buffers.end() && it->second.dsp) {
            const float userSpeed = XAudio2Hook::instance().getUserSpeed();
            const bool gate = XAudio2Hook::instance().isLengthGateEnabled();
            const float gateSeconds = XAudio2Hook::instance().lengthGateSeconds();

            const std::size_t frames = (combined.size() / sizeof(std::int16_t)) /
                                       std::max<std::uint32_t>(1, it->second.channels);
            const float durationSec =
                static_cast<float>(frames) / static_cast<float>(std::max<std::uint32_t>(1, it->second.sampleRate));
            if (!gate || durationSec <= gateSeconds) {
                auto out = it->second.dsp->process(combined.data(), combined.size(), userSpeed);
                if (!out.empty()) {
                    if (out.size() >= combined.size()) {
                        std::copy_n(out.data(), combined.size(), combined.begin());
                    } else {
                        std::copy(out.begin(), out.end(), combined.begin());
                        std::fill(combined.begin() + out.size(), combined.end(), 0);
                    }
                }
            }
        }
    }

    // Write combined buffer back into the two regions.
    std::size_t cursor = 0;
    if (pAudioPtr1 && dwAudioBytes1) {
        std::memcpy(pAudioPtr1, combined.data(), dwAudioBytes1);
        cursor += dwAudioBytes1;
    }
    if (pAudioPtr2 && dwAudioBytes2) {
        std::memcpy(pAudioPtr2, combined.data() + cursor, dwAudioBytes2);
    }

    return hook.m_origUnlock(self, pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
}

HRESULT WINAPI DirectSoundHook::UnlockHook8(IDirectSoundBuffer8 *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                            LPVOID pAudioPtr2, DWORD dwAudioBytes2) {
    return UnlockHook(reinterpret_cast<IDirectSoundBuffer *>(self), pAudioPtr1, dwAudioBytes1, pAudioPtr2, dwAudioBytes2);
}

} // namespace krkrspeed
