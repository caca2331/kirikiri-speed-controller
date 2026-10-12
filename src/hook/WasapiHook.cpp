#include "WasapiHook.h"
#include "WasapiFormat.h"
#include "DirectSoundHook.h"
#include "HookUtils.h"
#include "SharedSettingsManager.h"
#include "SharedStatusManager.h"
#include "../common/Logging.h"
#include "../common/AudioStreamProcessor.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <mmreg.h>
#include <atomic>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>
#include <memory>
#include <cmath>
#include <cstring>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace krkrspeed {

namespace {
using wasapi::StreamContext;

std::atomic<bool> g_loggedCoCreate{false};
std::atomic<bool> g_loggedEnumPatch{false};
std::atomic<bool> g_loggedDevicePatch{false};
std::atomic<bool> g_loggedAudioClientPatch{false};
std::atomic<bool> g_loggedRenderClientPatch{false};
std::atomic<bool> g_loggedAudioInit{false};
std::atomic<bool> g_loggedTempo{false};
std::atomic<bool> g_loggedSpeedup{false};
std::atomic<bool> g_loggedDsFallback{false};
std::atomic<bool> g_bootstrapDone{false};
std::atomic<bool> g_loggedSilenceGate{false};
std::atomic<bool> g_loggedFirstNonSilentDrop{false};
std::atomic<void *> g_bootstrapAudioClient{nullptr};

const GUID kClsidMMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
using PFN_GetDefaultAudioEndpoint =
    HRESULT(STDMETHODCALLTYPE *)(IMMDeviceEnumerator *, EDataFlow, ERole, IMMDevice **);
using PFN_GetDevice = HRESULT(STDMETHODCALLTYPE *)(IMMDeviceEnumerator *, LPCWSTR, IMMDevice **);
using PFN_DeviceActivate = HRESULT(STDMETHODCALLTYPE *)(IMMDevice *, REFIID, DWORD, PROPVARIANT *, void **);
using PFN_AudioClientInitialize =
    HRESULT(STDMETHODCALLTYPE *)(IAudioClient *, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME,
                                 const WAVEFORMATEX *, LPCGUID);
using PFN_AudioClientGetService = HRESULT(STDMETHODCALLTYPE *)(IAudioClient *, REFIID, void **);
using PFN_AudioClientRelease = ULONG(STDMETHODCALLTYPE *)(IAudioClient *);
using PFN_RenderClientGetBuffer = HRESULT(STDMETHODCALLTYPE *)(IAudioRenderClient *, UINT32, BYTE **);
using PFN_RenderClientReleaseBuffer = HRESULT(STDMETHODCALLTYPE *)(IAudioRenderClient *, UINT32, DWORD);
using PFN_RenderClientRelease = ULONG(STDMETHODCALLTYPE *)(IAudioRenderClient *);

PFN_GetDefaultAudioEndpoint g_origGetDefaultAudioEndpoint = nullptr;
PFN_GetDevice g_origGetDevice = nullptr;
PFN_DeviceActivate g_origDeviceActivate = nullptr;
PFN_AudioClientInitialize g_origAudioClientInitialize = nullptr;
PFN_AudioClientGetService g_origAudioClientGetService = nullptr;
PFN_AudioClientRelease g_origAudioClientRelease = nullptr;
PFN_RenderClientGetBuffer g_origRenderGetBuffer = nullptr;
PFN_RenderClientReleaseBuffer g_origRenderReleaseBuffer = nullptr;
PFN_RenderClientRelease g_origRenderClientRelease = nullptr;

struct RenderState {
    std::shared_ptr<StreamContext> ctx;
    BYTE *lastBuffer = nullptr;
    UINT32 lastFrames = 0;
    bool seenNonSilent = false;
    bool droppedFirstNonSilent = false;
};

// Entries are removed by the Release hooks when the COM object dies.
std::mutex g_ctxMutex;
std::unordered_map<IAudioClient *, std::shared_ptr<StreamContext>> g_audioClients;
std::unordered_map<IAudioRenderClient *, RenderState> g_renderClients;

std::shared_ptr<StreamContext> ensureRenderContextLocked(IAudioRenderClient *client) {
    auto &entry = g_renderClients[client];
    if (!entry.ctx) {
        entry.ctx = wasapi::createContextFromDefault();
    }
    return entry.ctx;
}

void updateRenderStateFlag(IAudioRenderClient *client, bool RenderState::*flag, bool value) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    auto it = g_renderClients.find(client);
    if (it != g_renderClients.end()) {
        it->second.*flag = value;
    }
}

void patchRenderClient(IAudioRenderClient *client);
void patchAudioClient(IAudioClient *client);
void patchDevice(IMMDevice *device);
void patchEnumerator(IMMDeviceEnumerator *enumerator);

HRESULT STDMETHODCALLTYPE RenderClientGetBufferHook(IAudioRenderClient *client, UINT32 numFramesRequested,
                                                     BYTE **ppData) {
    const HRESULT hr = g_origRenderGetBuffer ? g_origRenderGetBuffer(client, numFramesRequested, ppData) : E_FAIL;
    if (SUCCEEDED(hr) && ppData) {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        auto &entry = g_renderClients[client];
        entry.lastBuffer = *ppData;
        entry.lastFrames = numFramesRequested;
        if (!entry.ctx) {
            entry.ctx = wasapi::createContextFromDefault();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE RenderClientReleaseBufferHook(IAudioRenderClient *client, UINT32 numFramesWritten, DWORD flags) {
    if (!g_origRenderReleaseBuffer) return E_FAIL;
    RenderState state;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        ensureRenderContextLocked(client);
        auto &entry = g_renderClients[client];
        state = entry;
        entry.lastBuffer = nullptr;
        entry.lastFrames = 0;
    }
    SharedSettingsManager::instance().pollSharedSettings();

    auto ctx = state.ctx;
    if (!ctx) {
        return g_origRenderReleaseBuffer(client, numFramesWritten, flags);
    }
    std::unique_lock<std::mutex> ctxLock(ctx->mutex);

    // Format probes are COM round trips; run each at most once per stream instead of every period.
    if (!ctx->channelProbeDone) {
        ctx->channelProbeDone = true;
        wasapi::tryResolveChannelCountFromRenderClient(client, *ctx);
    }
    bool resolved = false;
    if (ctx->formatGuessed && !ctx->formatProbeDone) {
        ctx->formatProbeDone = true;
        resolved = wasapi::tryResolveFormatFromRenderClient(client, *ctx);
    }
    if (!resolved && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
        wasapi::maybeAdjustGuessedFormat(*ctx, state.lastBuffer, numFramesWritten);
    }

    if (DirectSoundHook::instance().isActive()) {
        ctx->budget.reset();
        ctxLock.unlock();
        if (!g_loggedDsFallback.exchange(true)) {
            KRKR_LOG_INFO("WASAPI fallback disabled: DirectSound activity detected");
        }
        return g_origRenderReleaseBuffer(client, numFramesWritten, flags);
    }
    const float speed = SharedSettingsManager::instance().getUserSpeed();
    const bool speedupActive = speed > 1.01f;
    UINT32 effectiveFrames = numFramesWritten;
    bool dropFrames = false;

    if (!speedupActive) {
        ctx->budget.reset();
        ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, numFramesWritten, flags);
    }

    if (numFramesWritten > 0) {
        effectiveFrames = ctx->budget.plan(numFramesWritten, speed, ctx->sampleRate);
        dropFrames = (effectiveFrames < numFramesWritten);
    }

    if (dropFrames && !g_loggedSpeedup.exchange(true)) {
        KRKR_LOG_INFO("WASAPI speedup drop mode active; reducing ReleaseBuffer frames");
    }

    if (numFramesWritten == 0) {
        ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, numFramesWritten, flags);
    }

    if (!state.lastBuffer || state.lastFrames == 0) {
        ctx->budget.commit(effectiveFrames);
        ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, effectiveFrames, flags);
    }

    const bool flaggedSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    const bool sampleSilent = flaggedSilent ? true : wasapi::isBufferSilent(*ctx, state.lastBuffer, numFramesWritten);
    if (!state.seenNonSilent && sampleSilent) {
        if (!g_loggedSilenceGate.exchange(true)) {
            KRKR_LOG_INFO("WASAPI initial silence gate: zeroing buffer until first non-silent audio");
        }
        std::memset(state.lastBuffer, 0, static_cast<std::size_t>(numFramesWritten) * ctx->blockAlign);
        ctx->budget.commit(effectiveFrames);
        ctxLock.unlock();
        updateRenderStateFlag(client, &RenderState::seenNonSilent, false);
        return g_origRenderReleaseBuffer(client, effectiveFrames, flags | AUDCLNT_BUFFERFLAGS_SILENT);
    }

    if (!state.seenNonSilent && !state.droppedFirstNonSilent && !sampleSilent) {
        const float durSec = (ctx->sampleRate > 0)
            ? static_cast<float>(numFramesWritten) / static_cast<float>(ctx->sampleRate)
            : 0.0f;
        if (durSec > 0.0f && durSec < 1.0f) {
            if (!g_loggedFirstNonSilentDrop.exchange(true)) {
                KRKR_LOG_INFO("WASAPI dropping first non-silent buffer to avoid startup click");
            }
            std::memset(state.lastBuffer, 0, static_cast<std::size_t>(numFramesWritten) * ctx->blockAlign);
            ctx->budget.commit(effectiveFrames);
            ctxLock.unlock();
            updateRenderStateFlag(client, &RenderState::droppedFirstNonSilent, true);
            return g_origRenderReleaseBuffer(client, effectiveFrames, flags | AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    if (!sampleSilent) {
        SharedStatusManager::instance().setActiveBackend(AudioBackend::Wasapi);
    }

    wasapi::ensureStream(*ctx);
    if (flaggedSilent || !ctx->stream) {
        ctx->budget.commit(effectiveFrames);
        ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, effectiveFrames, flags);
    }

    // The stream works in the endpoint's native sample format, so the buffer is processed in place.
    const std::size_t inputBytes = static_cast<std::size_t>(numFramesWritten) * ctx->blockAlign;
    const std::size_t outputBytes = static_cast<std::size_t>(effectiveFrames) * ctx->blockAlign;
    const float durationSec = (ctx->sampleRate > 0)
        ? static_cast<float>(numFramesWritten) / static_cast<float>(ctx->sampleRate)
        : 0.0f;
    const auto key = reinterpret_cast<std::uintptr_t>(client);

    ctx->stream->resetIfIdle(std::chrono::steady_clock::now(), std::chrono::milliseconds(200), false, key);
    auto &res = ctx->processed;
    ctx->stream->processTempoToSize(state.lastBuffer, inputBytes, outputBytes, speed, false, key, res);
    if (!res.output.empty()) {
        std::memcpy(state.lastBuffer, res.output.data(), std::min(res.output.size(), outputBytes));
    }
    ctx->stream->recordPlaybackEnd(durationSec, speed);
    ctx->budget.commit(effectiveFrames);

    if (!g_loggedTempo.exchange(true)) {
        KRKR_LOG_INFO("WASAPI tempo speedup active speed=" + std::to_string(speed) +
                      " cbuf=" + std::to_string(res.cbufferSize));
    }

    ctxLock.unlock();
    if (!state.seenNonSilent) {
        updateRenderStateFlag(client, &RenderState::seenNonSilent, true);
    }
    return g_origRenderReleaseBuffer(client, effectiveFrames, flags & ~AUDCLNT_BUFFERFLAGS_SILENT);
}

ULONG STDMETHODCALLTYPE RenderClientReleaseHook(IAudioRenderClient *client) {
    const ULONG remaining = g_origRenderClientRelease ? g_origRenderClientRelease(client) : 0;
    if (remaining == 0) {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        g_renderClients.erase(client);
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE AudioClientGetServiceHook(IAudioClient *client, REFIID riid, void **ppv) {
    const HRESULT hr = g_origAudioClientGetService ? g_origAudioClientGetService(client, riid, ppv) : E_FAIL;
    if (SUCCEEDED(hr) && ppv && *ppv && IsEqualIID(riid, __uuidof(IAudioRenderClient))) {
        if (!g_loggedRenderClientPatch.exchange(true)) {
            KRKR_LOG_INFO("WASAPI IAudioClient::GetService returned IAudioRenderClient");
        }
        auto *renderClient = reinterpret_cast<IAudioRenderClient *>(*ppv);
        patchRenderClient(renderClient);
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        auto it = g_audioClients.find(client);
        if (it != g_audioClients.end()) {
            // Fresh state: an address reused from an earlier render client must not inherit its flags.
            RenderState state{};
            state.ctx = it->second;
            g_renderClients[renderClient] = std::move(state);
        }
    }
    return hr;
}

ULONG STDMETHODCALLTYPE AudioClientReleaseHook(IAudioClient *client) {
    const ULONG remaining = g_origAudioClientRelease ? g_origAudioClientRelease(client) : 0;
    if (remaining == 0) {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        g_audioClients.erase(client);
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE AudioClientInitializeHook(IAudioClient *client, AUDCLNT_SHAREMODE shareMode, DWORD streamFlags,
                                                     REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity,
                                                     const WAVEFORMATEX *format, LPCGUID sessionGuid) {
    if (!g_loggedAudioInit.exchange(true)) {
        std::string msg = "WASAPI IAudioClient::Initialize";
        msg += (shareMode == AUDCLNT_SHAREMODE_SHARED) ? " shared" : " exclusive";
        if (format) {
            msg += " ch=" + std::to_string(format->nChannels) +
                   " rate=" + std::to_string(format->nSamplesPerSec) +
                   " bits=" + std::to_string(format->wBitsPerSample) +
                   " tag=" + std::to_string(format->wFormatTag);
            if (client == g_bootstrapAudioClient.load()) {
                msg += " src=bootstrap";
            }
        }
        KRKR_LOG_INFO(msg);
    }
    if (format) {
        wasapi::maybeSetDefaultFormat(format);
        auto ctx = wasapi::createContextFromFormat(format);
        if (!ctx->isPcm16 && !ctx->isPcm32 && !ctx->isFloat32) {
            KRKR_LOG_WARN("WASAPI format not PCM16/PCM32/Float32; processing disabled for this stream");
        }
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        g_audioClients[client] = std::move(ctx);
    }
    return g_origAudioClientInitialize ? g_origAudioClientInitialize(client, shareMode, streamFlags, hnsBufferDuration,
                                                                      hnsPeriodicity, format, sessionGuid)
                                       : E_FAIL;
}

HRESULT STDMETHODCALLTYPE DeviceActivateHook(IMMDevice *device, REFIID iid, DWORD clsctx, PROPVARIANT *params, void **ppv) {
    const HRESULT hr = g_origDeviceActivate ? g_origDeviceActivate(device, iid, clsctx, params, ppv) : E_FAIL;
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (IsEqualIID(iid, __uuidof(IAudioClient))) {
            patchAudioClient(reinterpret_cast<IAudioClient *>(*ppv));
        } else if (IsEqualIID(iid, __uuidof(IAudioRenderClient))) {
            if (!g_loggedRenderClientPatch.exchange(true)) {
                KRKR_LOG_INFO("WASAPI IMMDevice::Activate -> IAudioRenderClient");
            }
            patchRenderClient(reinterpret_cast<IAudioRenderClient *>(*ppv));
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpointHook(IMMDeviceEnumerator *enumerator, EDataFlow flow, ERole role,
                                                       IMMDevice **ppDevice) {
    const HRESULT hr = g_origGetDefaultAudioEndpoint ? g_origGetDefaultAudioEndpoint(enumerator, flow, role, ppDevice) : E_FAIL;
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        patchDevice(*ppDevice);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE GetDeviceHook(IMMDeviceEnumerator *enumerator, LPCWSTR id, IMMDevice **ppDevice) {
    const HRESULT hr = g_origGetDevice ? g_origGetDevice(enumerator, id, ppDevice) : E_FAIL;
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        patchDevice(*ppDevice);
    }
    return hr;
}

void patchEnumerator(IMMDeviceEnumerator *enumerator) {
    if (!enumerator) return;
    void **vtbl = *reinterpret_cast<void ***>(enumerator);
    bool patched = false;
    if (!g_origGetDefaultAudioEndpoint) {
        patched |= PatchVtableEntry(vtbl, 4, &GetDefaultAudioEndpointHook, g_origGetDefaultAudioEndpoint);
    }
    if (!g_origGetDevice) {
        patched |= PatchVtableEntry(vtbl, 5, &GetDeviceHook, g_origGetDevice);
    }
    if (patched && !g_loggedEnumPatch.exchange(true)) {
        KRKR_LOG_INFO("WASAPI IMMDeviceEnumerator vtable patched");
    }
}

void patchDevice(IMMDevice *device) {
    if (!device) return;
    void **vtbl = *reinterpret_cast<void ***>(device);
    bool patched = false;
    if (!g_origDeviceActivate) {
        patched |= PatchVtableEntry(vtbl, 3, &DeviceActivateHook, g_origDeviceActivate);
    }
    if (patched && !g_loggedDevicePatch.exchange(true)) {
        KRKR_LOG_INFO("WASAPI IMMDevice vtable patched");
    }
}

void patchAudioClient(IAudioClient *client) {
    if (!client) return;
    void **vtbl = *reinterpret_cast<void ***>(client);
    bool patched = false;
    if (!g_origAudioClientRelease) {
        patched |= PatchVtableEntry(vtbl, 2, &AudioClientReleaseHook, g_origAudioClientRelease);
    }
    if (!g_origAudioClientInitialize) {
        patched |= PatchVtableEntry(vtbl, 3, &AudioClientInitializeHook, g_origAudioClientInitialize);
    }
    if (!g_origAudioClientGetService) {
        patched |= PatchVtableEntry(vtbl, 14, &AudioClientGetServiceHook, g_origAudioClientGetService);
    }
    if (patched && !g_loggedAudioClientPatch.exchange(true)) {
        KRKR_LOG_INFO("WASAPI IAudioClient vtable patched");
    }
}

void patchRenderClient(IAudioRenderClient *client) {
    if (!client) return;
    void **vtbl = *reinterpret_cast<void ***>(client);
    bool patched = false;
    if (!g_origRenderClientRelease) {
        patched |= PatchVtableEntry(vtbl, 2, &RenderClientReleaseHook, g_origRenderClientRelease);
    }
    if (!g_origRenderGetBuffer) {
        patched |= PatchVtableEntry(vtbl, 3, &RenderClientGetBufferHook, g_origRenderGetBuffer);
    }
    if (!g_origRenderReleaseBuffer) {
        patched |= PatchVtableEntry(vtbl, 4, &RenderClientReleaseBufferHook, g_origRenderReleaseBuffer);
    }
    if (patched && !g_loggedRenderClientPatch.exchange(true)) {
        KRKR_LOG_INFO("WASAPI IAudioRenderClient vtable patched");
    }
}

void bootstrapWasapiVtables() {
    if (g_bootstrapDone.exchange(true)) {
        return;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        KRKR_LOG_WARN("WASAPI bootstrap: CoInitializeEx failed");
        return;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    hr = ::CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
                            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        KRKR_LOG_WARN("WASAPI bootstrap: CoCreateInstance(IMMDeviceEnumerator) failed");
        if (needUninit) {
            CoUninitialize();
        }
        return;
    }
    patchEnumerator(enumerator);

    IMMDevice *device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    }
    if (SUCCEEDED(hr) && device) {
        patchDevice(device);
    }

    IAudioClient *audioClient = nullptr;
    if (device) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                              reinterpret_cast<void **>(&audioClient));
    }
    if (SUCCEEDED(hr) && audioClient) {
        g_bootstrapAudioClient.store(audioClient);
        patchAudioClient(audioClient);
    }

    IAudioRenderClient *renderClient = nullptr;
    if (audioClient) {
        WAVEFORMATEX *mix = nullptr;
        if (SUCCEEDED(audioClient->GetMixFormat(&mix)) && mix) {
            wasapi::maybeSetDefaultFormat(mix);
            REFERENCE_TIME defaultPeriod = 0;
            if (FAILED(audioClient->GetDevicePeriod(&defaultPeriod, nullptr)) || defaultPeriod <= 0) {
                defaultPeriod = 1000000; // 100ms in 100ns units
            }
            const REFERENCE_TIME bufferDuration = defaultPeriod * 2;
            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_NOPERSIST,
                                         bufferDuration,
                                         0,
                                         mix,
                                         nullptr);
            CoTaskMemFree(mix);
            mix = nullptr;
            if (SUCCEEDED(hr)) {
                hr = audioClient->GetService(__uuidof(IAudioRenderClient),
                                             reinterpret_cast<void **>(&renderClient));
            }
        }
    }
    if (renderClient) {
        patchRenderClient(renderClient);
    } else {
        KRKR_LOG_WARN("WASAPI bootstrap: IAudioRenderClient not available");
    }

    if (renderClient) {
        renderClient->Release();
    }
    if (audioClient) {
        audioClient->Release();
    }
    if (device) {
        device->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
    if (needUninit) {
        CoUninitialize();
    }
    KRKR_LOG_INFO("WASAPI bootstrap vtable patch complete");
}

} // namespace

WasapiHook &WasapiHook::instance() {
    static WasapiHook hook;
    return hook;
}

void WasapiHook::initialize() {
    tryPatchModule(GetModuleHandleW(nullptr), "main");
    HMODULE unity = GetModuleHandleA("UnityPlayer.dll");
    if (unity) {
        tryPatchModule(unity, "UnityPlayer.dll");
    }
    bootstrapWasapiVtables();

    if (!m_origCoCreate) {
        m_origCoCreate = ::CoCreateInstance;
        if (!m_loggedInit.exchange(true)) {
            KRKR_LOG_DEBUG("WASAPI using CoCreateInstance fallback (no import patch)");
        }
    }
}

void WasapiHook::tryPatchModule(HMODULE module, const char *moduleName) {
    bool patched = false;
    if (!m_origCoCreate) {
        void *orig = nullptr;
        if (PatchImportInModule(module, "ole32.dll", "CoCreateInstance",
                                reinterpret_cast<void *>(&WasapiHook::CoCreateInstanceHook), &orig)) {
            m_origCoCreate = reinterpret_cast<PFN_CoCreateInstance>(orig);
            patched = true;
        }
    }
    if (!m_origCoCreate) {
        void *orig = nullptr;
        if (PatchImportInModule(module, "combase.dll", "CoCreateInstance",
                                reinterpret_cast<void *>(&WasapiHook::CoCreateInstanceHook), &orig)) {
            m_origCoCreate = reinterpret_cast<PFN_CoCreateInstance>(orig);
            patched = true;
        }
    }
    if (patched) {
        KRKR_LOG_INFO(std::string("WASAPI CoCreateInstance patched (") + moduleName + ")");
    }
}

void WasapiHook::setOriginalCoCreate(void *fn) {
    if (!fn || m_origCoCreate) {
        return;
    }
    m_origCoCreate = reinterpret_cast<PFN_CoCreateInstance>(fn);
}

HRESULT WINAPI WasapiHook::CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                                REFIID riid, LPVOID *ppv) {
    auto &self = WasapiHook::instance();
    auto fn = self.m_origCoCreate ? self.m_origCoCreate : ::CoCreateInstance;

    const HRESULT hr = fn(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && IsEqualCLSID(rclsid, kClsidMMDeviceEnumerator)) {
        if (!g_loggedCoCreate.exchange(true)) {
            KRKR_LOG_INFO("WASAPI CoCreateInstance -> IMMDeviceEnumerator");
        }
        // Only the IMMDeviceEnumerator interface has the vtable layout we patch.
        if (IsEqualIID(riid, __uuidof(IMMDeviceEnumerator))) {
            patchEnumerator(reinterpret_cast<IMMDeviceEnumerator *>(*ppv));
        } else {
            IMMDeviceEnumerator *enumerator = nullptr;
            auto *unknown = reinterpret_cast<IUnknown *>(*ppv);
            if (SUCCEEDED(unknown->QueryInterface(__uuidof(IMMDeviceEnumerator),
                                                  reinterpret_cast<void **>(&enumerator))) && enumerator) {
                patchEnumerator(enumerator);
                enumerator->Release();
            }
        }
    }
    return hr;
}

} // namespace krkrspeed
