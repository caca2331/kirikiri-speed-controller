#pragma once

#include <windows.h>
#include <dsound.h>
#include <map>
#include <mutex>
#include "../common/DspPipeline.h"
#include "../common/VoiceContext.h"

namespace krkrspeed {

class DirectSoundHook {
public:
    static DirectSoundHook &instance();
    void initialize();

    // Allow late binding when DirectSoundCreate8 is resolved dynamically.
    void setOriginalCreate8(void *fn);
    bool hasCreateHook() const { return m_origCreate8 != nullptr; }

    static HRESULT WINAPI DirectSoundCreate8Hook(LPCGUID pcGuidDevice, LPDIRECTSOUND8 *ppDS8, LPUNKNOWN pUnkOuter);
    static HRESULT WINAPI CreateSoundBufferHook(IDirectSound8 *self, LPDIRECTSOUNDBUFFER *ppDSBuffer,
                                                LPCDSBUFFERDESC pcDSBufferDesc);
    static HRESULT WINAPI UnlockHook(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1, LPVOID pAudioPtr2,
                                     DWORD dwAudioBytes2);
    static HRESULT WINAPI UnlockHook8(IDirectSoundBuffer8 *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                      LPVOID pAudioPtr2, DWORD dwAudioBytes2);

private:
    DirectSoundHook() = default;
    void hookEntryPoints();

    using PFN_DirectSoundCreate8 = HRESULT(WINAPI *)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);
    using PFN_CreateSoundBuffer = HRESULT(__stdcall *)(IDirectSound8 *, LPDIRECTSOUNDBUFFER *, LPCDSBUFFERDESC);
    using PFN_Unlock = HRESULT(__stdcall *)(IDirectSoundBuffer *, LPVOID, DWORD, LPVOID, DWORD);

    PFN_DirectSoundCreate8 m_origCreate8 = nullptr;
    PFN_CreateSoundBuffer m_origCreateBuffer = nullptr;
    PFN_Unlock m_origUnlock = nullptr;

    struct BufferInfo {
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
        std::unique_ptr<DspPipeline> dsp;
    };
    std::map<std::uintptr_t, BufferInfo> m_buffers;
    std::mutex m_mutex;
};

} // namespace krkrspeed
