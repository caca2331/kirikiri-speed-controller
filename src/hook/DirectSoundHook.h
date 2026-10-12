#pragma once

#include <windows.h>
#include <dsound.h>
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <chrono>
#include <vector>
#include "../common/AudioStreamProcessor.h"

namespace krkrspeed {

class DirectSoundHook {
public:
    static DirectSoundHook &instance();
    // Import-table patches only; cheap and safe to run from DllMain.
    void installImportHooks();
    // Remaining setup (module scan, vtable bootstrap, x86 Unlock detour); runs on the init thread.
    void initialize();

    // Allow late binding when DirectSoundCreate8 is resolved dynamically.
    void setOriginalCreate8(void *fn);
    void setOriginalCreate(void *fn);
    void scanLoadedModules();
    void bootstrapVtable();
    bool hasCreateHook() const { return m_origCreate8 != nullptr; }
    bool isActive() const { return m_active.load(); }

    void patchDeviceVtable(IDirectSound8 *ds8);
    void patchBufferVtable(IDirectSoundBuffer *buf);
    void installGlobalUnlockHook();

    static HRESULT WINAPI DirectSoundCreate8Hook(LPCGUID pcGuidDevice, LPDIRECTSOUND8 *ppDS8, LPUNKNOWN pUnkOuter);
    static HRESULT WINAPI DirectSoundCreateHook(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter);
    static HRESULT WINAPI CreateSoundBufferHook(IDirectSound8 *self, LPDIRECTSOUNDBUFFER *ppDSBuffer,
                                                LPCDSBUFFERDESC pcDSBufferDesc);
    static HRESULT WINAPI UnlockHook(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1, LPVOID pAudioPtr2,
                                     DWORD dwAudioBytes2);
    static ULONG __stdcall ReleaseHook(IDirectSoundBuffer *self);

    __declspec(noinline) HRESULT handleUnlock(IDirectSoundBuffer *self, LPVOID pAudioPtr1, DWORD dwAudioBytes1,
                                              LPVOID pAudioPtr2, DWORD dwAudioBytes2);

private:
    DirectSoundHook() = default;

    using PFN_DirectSoundCreate8 = HRESULT(WINAPI *)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);
    using PFN_DirectSoundCreate = HRESULT(WINAPI *)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
    using PFN_CreateSoundBuffer = HRESULT(__stdcall *)(IDirectSound8 *, LPDIRECTSOUNDBUFFER *, LPCDSBUFFERDESC);
    using PFN_Unlock = HRESULT(__stdcall *)(IDirectSoundBuffer *, LPVOID, DWORD, LPVOID, DWORD);
    using PFN_Release = ULONG(__stdcall *)(IDirectSoundBuffer *);

    PFN_DirectSoundCreate8 m_origCreate8 = nullptr;
    PFN_DirectSoundCreate m_origCreate = nullptr;
    PFN_CreateSoundBuffer m_origCreateBuffer = nullptr;
    PFN_Unlock m_origUnlock = nullptr;
    PFN_Release m_origRelease = nullptr;

    struct BufferInfo {
        std::mutex mutex; // serializes Unlock processing for this buffer only
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
        std::uint16_t bitsPerSample = 16;
        bool isPcm16 = true;
        std::uint16_t formatTag = WAVE_FORMAT_PCM;
        std::uint32_t baseFrequency = 0;
        std::uint32_t bufferBytes = 0;
        std::uint32_t blockAlign = 0;
        float approxSeconds = 0.0f;
        std::atomic<bool> isLikelyBgm{false};
        bool frequencyOverridden = false; // we changed the frequency and must restore it when DSP stops
        std::uint64_t unlockCount = 0;
        std::uint64_t processedFrames = 0;
        std::vector<std::uint8_t> combined;
        std::unique_ptr<AudioStreamProcessor> stream;
    };
    using BufferPtr = std::shared_ptr<BufferInfo>;

    static BufferPtr makeBufferInfo(const WAVEFORMATEX &fmt, DWORD bufferBytes, float bgmGateSeconds);
    void **shadowFor(void **origVtbl, std::size_t count, std::unordered_map<void **, std::vector<void *>> &shadows);
    BufferPtr findOrTrackBuffer(IDirectSoundBuffer *self);
    void rememberBgmRelease(std::uintptr_t key, std::chrono::steady_clock::time_point now);
    bool consumeRecentBgmRelease(std::uintptr_t key, std::chrono::steady_clock::time_point now);

    std::map<std::uintptr_t, BufferPtr> m_buffers;
    std::set<std::string> m_loggedFormats;
    std::mutex m_mutex;
    std::mutex m_vtableMutex;
    // Shadow vtables keyed by the original vtable they copy. They are shared by every object of that
    // class and never freed, so a recycled object address can neither skip patching nor dangle.
    std::unordered_map<void **, std::vector<void *>> m_deviceShadows;
    std::unordered_map<void **, std::vector<void *>> m_bufferShadows;
    std::set<void **> m_shadowTables;
    std::atomic<bool> m_importHooksInstalled{false};
    std::atomic<bool> m_loggedUnlockOnce{false};
    std::atomic<bool> m_disableAfterFault{false};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_seenMono{false};
    std::atomic<bool> m_seenStereo{false};
    std::atomic<bool> m_loggedMonoStereo{false};
    std::unordered_map<std::uintptr_t, std::chrono::steady_clock::time_point> m_bgmReleaseTimes;
};

} // namespace krkrspeed
