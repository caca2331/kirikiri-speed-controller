# Universal Kirikiri Voice Speed Controller – Implementation Guide (Dec 2025)

This guide is the “how it works” document for understanding, implementing, and re‑implementing the controller and hooks. It reflects the **current, tested logic**—DirectSound is the reference path; WASAPI should mirror it where possible (Unity titles included).

## 1. Objective
- Inject a DLL into target games and time‑stretch **speech** to match game speed (0.5×–2.0×) without pitch distortion and with minimal latency (<100 ms).

## 2. Stack & Build
- C++17, MSVC static CRT.
- SoundTouch (WSOLA) for DSP; defaults: sequence 35 ms, overlap 10 ms, seek 25 ms (tune per voice).
- MinHook for IAT/vtable patching.
- Builds: x86 mandatory, x64 optional. `dist_dual_arch` stages both.

## 3. Core Flow
```
Game → Hook (DirectSound/WASAPI) → DSP (SoundTouch) → Output device
```
Each intercepted buffer is frequency‑scaled (tempo) then pitch‑restored so the user speed is achieved without pitch shift.
Primary tracked interface is **DirectSound**. If no DirectSound buffers are observed, the WASAPI hook serves as the
fallback for untracked audio APIs (currently anything not routed through DirectSound).

## 4. AudioStreamProcessor (reference behavior)
- Cbuffer is **output FIFO** (already DSP’d). It is **not** reprocessed.
- Per Unlock (must fill Abuffer length immediately):
  1) Copy from Cbuffer into output until full or Cbuffer empty.
  2) Process **only the new slice** with SoundTouch (pitch mode, ratio = 1 / appliedSpeed).
  3) If DSP overproduces, stash excess into Cbuffer; if underproduces, **front‑pad zeros** to reach exact Abuffer size.
  4) Cbuffer has no hard cap; continuity is preserved and it is only cleared on idle reset.
- Tempo path (WASAPI): accumulate input in Abuffer until ~30 ms, then run SoundTouch in **tempo** mode. Output is filled from Cbuffer first, then new DSP output, and zero‑padded if needed.
- Idle reset: if idle >200 ms beyond predicted play end, clear Cbuffer/Abuffer and `flush()` SoundTouch state.

## 5. Two Processing Routes
### Route A — Voice/BGM distinguishable (DirectSound‑style)
- Identify likely voice vs BGM:
  - Mono buffers → voice‑leaning.
  - Long or stereo buffers → BGM‑leaning (length gate default 60 s).
  - Optional “process all audio” override.
- For **voice**:
  1) Compute `DesiredFreq = clamp(baseFreq * userSpeed, DSBFREQUENCY_[MIN,MAX])`.
  2) `AppliedSpeed = DesiredFreq / baseFreq`.
  3) Run **pitch mode** in AudioStreamProcessor with `ratio = 1 / AppliedSpeed` to restore pitch.
  4) Write back to the locked regions; overflow goes into Cbuffer (not re‑DSP’d).
  5) Enforce `SetFrequency(DesiredFreq)` each unlock to override game resets.
- For **BGM**:
  - Default: bypass DSP and keep base frequency.
  - If `processAllAudio`, use the same pipeline as voice.

### Route B — Voice/BGM indistinguishable (WASAPI‑style)
- Used when no tracked interface is active (currently only DirectSound).
- Always process shared-mode PCM16/PCM32/float32 audio in its native format (no BGM gate). Exclusive-mode streams are passed through: the device plays every requested frame, so dropping frames would play stale data.
- Hook points: `IAudioClient::Initialize` → `IAudioRenderClient::GetBuffer/ReleaseBuffer`.
- Per `ReleaseBuffer`:
  1) Compute `effectiveFrames` via the accumulator (`targetFrames/outFrames`) so long‑term output tracks `sum(inputFrames / speed)`.
  2) Run **tempo mode** (`SoundTouch::setTempo`) through `processTempoToSize`:
     - Abuffer accumulates until ~30 ms before DSP to stabilize SoundTouch.
     - Output size is exactly `effectiveFrames` (Cbuffer + new DSP output + zero padding if needed).
  3) Release only `effectiveFrames` (drop mode) to speed up playback without pitch shift.
- Speed‑down is not supported in this route without a proxy render client (would require buffering and backpressure).

## 6. DirectSound Hook (tested)
- Hook points: DirectSoundCreate/DirectSoundCreate8 → CreateSoundBuffer → Unlock/Release (secondary PCM16 only).
- Classification: mono → likely voice; long/stereo → likely BGM; length gate default 60 s; optional process‑all‑audio.
- Speed control per Unlock:
  - DesiredFreq = clamp(baseFreq * userSpeed, DSBFREQUENCY_[MIN,MAX]).
  - Always `GetFrequency` and `SetFrequency` if mismatch (overrides games that keep resetting frequency).
  - AppliedSpeed = DesiredFreq / baseFreq; feed this to SoundTouch (pitch mode) in AudioStreamProcessor.
- DSP result is written back to the locked regions; tail is kept in Cbuffer (not re‑DSPed).
- Interception: each device/buffer created through the hooked `DirectSoundCreate*` gets a shadow vtable (shared per original vtable). On x86 an inline detour on dsound's Unlock also catches buffers created before injection; those are tracked lazily, and the primary buffer is passed through (its entry is re-validated with `GetCaps`, since its vtable is not patched). x64 has no such detour, so devices created before injection are not hooked (see FOLLOW_UP.md).
- The locked regions are trusted as returned by Lock (no `IsBadReadPtr`/`IsBadWritePtr` probing).
- Logs: `--log` enables; the controller writes `krkr_controller.log`, each hooked process `krkr_hook_<pid>.log` (directory from the `%TEMP%/krkr_log_dir.txt` hint, UTF-8).

## 7. Controller (KrkrSpeedController.exe)
- Enumerates visible processes, writes shared settings (speed, gates, stereo‑BGM mode, process‑all) to per‑PID shared memory, launches injector, and reports status.
- CLI: `--log`, `--speed <x>`, `--mark-stereo-bgm <aggressive|hybrid|none>`, `--bgm-secs <N>`, `--search <term>`, `--launch <exe>` / `-l <exe>`.
- Config `krkr_speed_config.yaml` beside the controller: `controller.minimize_to_tray_on_close`, `controller.ui_language`, `controller.last_speed` (startup speed unless `--speed` is given; saved when the speed box, a hotkey or Hook sets a new speed, but a `--speed` value only once the user has changed the speed), `auto_hook` and `process_bgm` lists.
- UI: Common Controls v6 and PerMonitorV2 DPI awareness come from `KrkrSpeedController.manifest`. Layout values are written for 96 DPI and pass through `scalePx`; fonts are rebuilt on `WM_DPICHANGED`.
- The injector runs with `CREATE_NO_WINDOW`.

## 8. Injection & Shared Settings
- Injector writes the hook DLL path into remote process via LoadLibraryW. The remote thread's exit code holds only the low 32 bits of the HMODULE on x64, so an exit code of 0 is confirmed against the target's module list before reporting failure. If the remote call times out, the path buffer is not freed and the controller does not retry other paths; a per-PID mutex (`Local\KrkrSpeedHook_<pid>`) keeps any second hook copy inert.
- SoundTouch.dll is delay-loaded from the hook DLL's folder (`DspRuntime.cpp`); copying the hook beside the game is only a fallback.
- Import-table hooks are installed inside DllMain; COM/DirectSound bootstrap runs on the init thread.
- Shared settings name: `Local\KrkrSpeedSettings_<pid>`; fields (`SharedSettings.h`): userSpeed, lengthGateSeconds, lengthGateEnabled, enableLog, disableBgm, processAllAudio, bgmSecondsGate, stereoBgmMode, layoutSize, controllerVersion. New fields are appended only.
- Logging: DEBUG lines are buffered and flushed when 1 s has passed since the last flush (or by the next INFO+ line); INFO and above flush at once. `FlushLog()` (try-lock, never blocks) runs from the hook's vectored handler and on `DLL_PROCESS_DETACH`.
- Hook reads settings at attach; DS/WASAPI polls periodically for updates.

## 9. Stability Practices
- Never block audio threads; fail soft: on exceptions, disable processing for that path.
- Validate buffer sizes and formats; guard SetFrequency/Submit failures with logs.
- Keep buffers alive until engine callbacks signal completion (Unlock path in DS; render client lifetimes in WASAPI).

## 10. Known Gaps / To‑Do
- More coverage on Unity titles that rely on WASAPI timing quirks.
- Further tuning of SoundTouch params for very short slices (<60 ms).
- Open ideas and known issues with their trade-offs are tracked in `FOLLOW_UP.md`.

Use this guide to reimplement or extend the controller: follow the DS hook and AudioStreamProcessor behaviors as the canonical reference. 
