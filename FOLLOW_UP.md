# Follow-up ideas with trade-offs

These were left out on purpose. Each changes what users see or hear, or adds risk or maintenance cost. Pick them one at a time; the "When" line says what should be true first.

Items that start with **Problem:** come from a code review (2026-11) and have not been reproduced in a game yet.

Done since this list was first written: app manifest (comctl32 v6 + PerMonitorV2 DPI), remembered last speed, removal of `IsBadReadPtr`/`IsBadWritePtr` from Unlock, batched log flushing, x64 injector exit-code check, no SoundTouch.dll beside the controller in `dist` (see CHANGELOG.md).

## UI

### Configurable hotkeys (defaults unchanged)
- **Gain:** avoids clashes with other apps that grab `Alt+'`, `Alt+[`, `Alt+]`.
- **Cost:** a config syntax for key names, validation, and UI text in three languages; more support questions.
- **How:** `controller.hotkeys` section in the YAML, parsed into the `RegisterHotKey` calls in `registerControllerHotkeys` (`ui_widgets.cpp`); show the active keys in the hotkey label.

## Audio

### Process `WAVE_FORMAT_EXTENSIBLE` 16-bit PCM in DirectSound
- **Gain:** covers games that describe plain PCM with the extensible header.
- **Cost:** buffers that are passed through untouched today would start being processed. Multichannel layouts (5.1) would then go through SoundTouch too.
- **How:** in `makeBufferInfo` (`DirectSoundHook.cpp`) accept `WAVE_FORMAT_EXTENSIBLE` when `SubFormat == KSDATAFORMAT_SUBTYPE_PCM` and `wBitsPerSample == 16`; optionally restrict to 1–2 channels.

### Expose SoundTouch tuning (sequence / overlap / seek window)
- **Gain:** better quality on short voice clips for people who tune it.
- **Cost:** wrong values sound worse than the defaults; more settings to explain.
- **How:** `DspConfig` already carries the three values; add them to `SharedSettings` (append at the end) and pass them when `AudioStreamProcessor` is created.

### WASAPI slow-down (speeds below 1.0)
- **Gain:** parity with DirectSound.
- **Cost:** dropping frames cannot slow audio down. It needs a proxy render client that owns its own buffer, feeds the real endpoint, and handles back-pressure (`GetCurrentPadding`, event-driven clients). Large and risky.

### Idle-gap reset of the length gate
- **Gain:** a reused Kirikiri buffer that plays many short voice lines would not eventually be classified as BGM by `processedFrames` accumulating across lines.
- **Cost:** behaviour change; a long BGM with silent gaps could be re-timed from zero after each gap.
- **When:** only if logs show voice buffers flipping to BGM (`DS buffer marked BGM via length gate` with a voice-sized buffer).

### x64 DirectSound: devices created before injection are not hooked
- **Problem:** only objects created through the hooked `DirectSoundCreate*` get a shadow vtable. x86 also has an inline Unlock detour as a catch-all; x64 has none. Injecting into an x64 game that already created its device (manual Hook or auto-hook after startup) processes nothing; buffers from `DuplicateSoundBuffer` are missed on x64 as well. `--launch` is not affected.
- **How:** patch the Unlock/Release slots of dsound's shared buffer vtable in place (as `PatchVtableEntry` does for WASAPI), on both architectures; this could replace the x86 inline detour. Unknown buffers then go through `findOrTrackBuffer`.
- **Cost:** touches every DirectSound buffer in the process, including ones the game created earlier; needs testing on x86 and x64 Kirikiri titles.

### WASAPI: guessed stream format can over-read/over-write the buffer
- **Problem:** when `IAudioClient::Initialize` was missed (injection after audio started), the format comes from the default mix format (usually float32). If the stream is really PCM16 (`AUTOCONVERTPCM`), `processTempoToSize` reads and writes `frames × 8` bytes in a region sized `frames × 4`. `maybeAdjustGuessedFormat` usually corrects the guess, but not before the first processed period.
- **How:** do not run DSP on a context while `formatGuessed` is set (pass through until the format is confirmed by the sample heuristic or a probe).
- **Cost:** late-injected streams start speeding up a little later; needs logs from a Unity title injected mid-game.

### DirectSound activity switches WASAPI processing off globally
- **Problem:** any DirectSound Unlock in the process (e.g. a DirectShow movie renderer in an otherwise WASAPI game) turns the WASAPI speed-up off for good. Conversely, after `m_disableAfterFault` the WASAPI hook can start processing dsound's own internal render stream.
- **How:** decide per stream, e.g. skip only the `IAudioClient` that dsound.dll itself creates (check the caller module in the Initialize hook), instead of the global `DirectSoundHook::isActive()` flag.
- **Cost:** must not double-process DirectSound games; needs testing on both kinds of titles.

### One-shot DirectSound buffers lose their last few tens of ms
- **Problem:** pitch-mode SoundTouch has latency and the stream starts with 30 ms of padding. A static buffer filled by a single Unlock never gets its tail written. Kirikiri streams through ring buffers and is not affected; other engines may clip line endings.
- **How:** for buffers whose whole size was written in one Unlock, compensate the latency (e.g. pre-roll, or skip padding) — design needed.

## Safety and robustness

### Import patching assumes an Import Lookup Table
- **Problem:** `PatchImportInModule` (`HookUtils.cpp`) reads `OriginalFirstThunk` unconditionally. Some older linkers and packers leave it 0; the code then walks the DOS header as if it were a name table, which can fault inside DllMain.
- **How:** if `OriginalFirstThunk == 0`, walk `FirstThunk` and compare each bound address with `GetProcAddress` of the wanted function; bounds-check RVAs against `SizeOfImage`.
- **When:** cheap to do; worth it before the x64 vtable change above.

### Atomic hot-patch for the x86 Unlock detour
- **Problem:** the 5-byte jump is written over the live prologue in one `memcpy`; a thread entering Unlock at that moment could execute a torn instruction.
- **How:** use the hot-patch layout: write the long jump into the 5 padding bytes before the function, then replace `mov edi,edi` with a 2-byte short jump in one aligned write.

### Smaller items
- Buffers whose final Release goes through another interface (`IDirectSound3DBuffer`, `IDirectSoundNotify`) never reach `ReleaseHook`, so their entry stays in `m_buffers` until the address is reused.
- The hook can turn logging on from shared settings but never off.
- `krkr_speed_config.yaml` is rewritten in place (truncate, then write); a crash mid-write loses it. Write a temporary file and rename it. It also lives beside the exe, which fails under Program Files.
- `IAudioClient3::InitializeSharedAudioStream` is not hooked; streams initialized that way get a guessed format.
- On ARM64 Windows, an emulated x64 game is reported as ARM64 (`IsWow64Process2` gives no WOW64 machine for x64 emulation), so no hook is found.

## Distribution

### Clean up old hook copies left in game folders
- **Gain:** tidy game folders after older versions copied `krkr_speed_hook.dll` there.
- **Cost:** deletes files inside the user's game directory. It must never touch `SoundTouch.dll`, which may belong to the game. A README note is the safer option.

### Code signing
- **Gain:** fewer antivirus false positives and SmartScreen prompts.
- **Cost:** a certificate costs money every year; an injector will still be flagged by some engines.

### Event-driven process detection
- **Gain:** auto-hook reacts instantly instead of within 3 s.
- **Cost:** WMI (`Win32_ProcessStartTrace`, needs admin) or ETW, both much more code than the 3 s poll, which is already cheap. Not recommended.

### Tests and CI
- Extract the DirectSound voice/BGM classification from `handleUnlock` into a pure function with unit tests (like `FrameDropBudget.h`); it is the heuristic most likely to regress.
- The `package` job rebuilds both architectures that the `windows` jobs already built; a tag-triggered release job could publish the zipped `dist` folder.
