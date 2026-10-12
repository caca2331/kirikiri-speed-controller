# Follow-up ideas with trade-offs

These were left out of the trade-off-free batch on purpose. Each changes what users see or hear, or adds risk or maintenance cost. Pick them one at a time; the "When" line says what should be true first.

## UI

### App manifest (comctl32 v6, DPI awareness)
- **Gain:** sharp text on high-DPI screens; the GitHub link becomes a real clickable SysLink. Today `WC_LINK` fails without comctl32 v6, so `ui.cpp` falls back to a static label (`g_linkIsSysLink = false`).
- **Cost:** every control switches to the themed visual style. Per-monitor DPI awareness also needs `layoutControls` (`ui_widgets.cpp`) and the hard-coded pixel offsets in `WM_CREATE` (`ui.cpp`) to scale by `GetDpiForWindow`; otherwise controls overlap at 150%+.
- **How:** add a `.manifest` with the `Microsoft.Windows.Common-Controls` 6.0 dependency (and optionally `<dpiAwareness>PerMonitorV2</dpiAwareness>`) and list it in the controller's sources. Could be done in two steps: manifest without DPI awareness first, then DPI.

### Remember the last speed between sessions
- **Gain:** convenience.
- **Cost:** startup no longer always means 1.5x unless `--speed` is given; users who rely on that default get surprised. Needs a rule for which wins (`--speed` should).
- **How:** save `controller.last_speed` in `krkr_speed_config.yaml` from `applySpeedToHookedProcesses`/hotkeys, read it in `setInitialOptions` when `--speed` is absent.

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

### Idle-gap reset of the length gate (item 3 nuance)
- **Gain:** a reused Kirikiri buffer that plays many short voice lines would not eventually be classified as BGM by `processedFrames` accumulating across lines.
- **Cost:** behaviour change; a long BGM with silent gaps could be re-timed from zero after each gap.
- **When:** only if logs show voice buffers flipping to BGM (`DS buffer marked BGM via length gate` with a voice-sized buffer).

## Safety and robustness

### Remove `IsBadReadPtr` / `IsBadWritePtr` from Unlock
- **Gain:** these are deprecated and run on every Unlock; they can also swallow guard-page exceptions.
- **Cost:** loses a safety net for a game that passes pointers DirectSound did not hand out.
- **How:** delete the `ptrInvalid` check at the top of `handleUnlock` (`DirectSoundHook.cpp`).

### Flush the log with `'\n'` instead of `std::endl`
- **Gain:** much cheaper log writes when `--log` is on.
- **Cost:** the last lines before a crash can be lost, exactly when they matter most.
- **How:** in `writeLine` (`Logging.cpp`); possibly flush only on `Warn`/`Error`.

### Fix x64 injector exit-code truncation
- **Gain:** the injector reports the real `LoadLibraryW` result for x64 targets (the thread exit code holds only the low 32 bits of the module handle).
- **Cost:** needs a module enumeration in the target (`EnumProcessModulesEx`) after the remote thread ends. Already mitigated: the per-process mutex keeps a second hook inert.

## Distribution

### Remove the root `SoundTouch.dll` from `dist`
- **Gain:** one file fewer to ship.
- **When:** only after a Windows build confirms the hook links with `/DELAYLOAD:SoundTouch.dll`. The controller no longer links SoundTouch, but the 32-bit preflight `LoadLibraryW` of the x86 hook may still resolve it from the controller folder in some setups. Check with the preflight log before removing it from `cmake/dual_arch.cmake`.

### Clean up old hook copies left in game folders
- **Gain:** tidy game folders after older versions copied `krkr_speed_hook.dll` there.
- **Cost:** deletes files inside the user's game directory. It must never touch `SoundTouch.dll`, which may belong to the game. A README note is the safer option.

### Code signing
- **Gain:** fewer antivirus false positives and SmartScreen prompts.
- **Cost:** a certificate costs money every year; an injector will still be flagged by some engines.

### Event-driven process detection
- **Gain:** auto-hook reacts instantly instead of within 3 s.
- **Cost:** WMI (`Win32_ProcessStartTrace`, needs admin) or ETW, both much more code than the 3 s poll, which is already cheap. Not recommended.
