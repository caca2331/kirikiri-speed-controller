## [Unreleased]
### Fixed
- Controller no longer overflows a stack buffer when a process (e.g. a browser tab) has a very long window title; dropdown entries carry their PID as item data
- Injection timeouts are no longer reported as failures that trigger retries from other paths; the injector keeps the remote path buffer alive after a timeout; a per-process guard keeps a second hook copy inert
- x64 DirectSound games: hooks are no longer skipped when the game's device reuses the address of the bootstrap device (shadow vtables are shared per class)
- Hook logging: re-entrant logging from the crash handler is dropped, only fatal exceptions are recorded, codes are printed in hex
- Hook log location works with non-ASCII controller paths; each hooked process writes its own `krkr_hook_<PID>.log`
- Buffers that are not sped up keep the game's own frequency (it is only restored once after we stop processing a buffer)
- x86 Unlock detour validates the function prologue and routes through the trampoline before going live
### Changed
- SoundTouch.dll is delay-loaded from the hook folder; the hook and SoundTouch.dll are copied next to the game only as a fallback (an existing SoundTouch.dll there is never overwritten)
- Import hooks are installed synchronously in DllMain, so `--launch` games are hooked before their first instruction runs
- Hook/Launch run on worker threads; the window stays responsive during injection and elevation prompts
- Hotkeys and the speed box apply to all hooked games
- WASAPI processes float/PCM32 streams natively (no 16-bit round trip); COM format probes run once per stream
- DirectSound processing holds only a per-buffer lock; settings come from one shared snapshot
- Split `ui.cpp`, `ControllerCore.cpp` and `WasapiHook.cpp` below 1000 lines; removed dead code
### Added
- DSP unit tests (`-DBUILD_TESTS=ON`, `ctest`); they also build on non-Windows hosts with a fallback DSP

## [1.2.1] - 2026-01-12
### Added
- "Minimize to tray on close" option with tray icon and Exit menu
- Info label with version tooltip and localized warning text
- Second-instance behavior now restores and focuses the existing window (including tray/minimized)
- Added app icon
- Status box now logs detected audio backend (DirectSound/WASAPI) once per hooked game
### Changed
- Checkbox labels and main labels are right-aligned with tighter spacing
- Auto-hook delay controls now stay visible and are disabled when auto-hook is off
- Added a new row under the status box for the tray option and tweaked label widths/spacing
### Fixed
- Controller background no longer turns black after minimize/restore

## [1.2.0] - 2026-01-03
### Added
- WASAPI hook with tempo-based speedup path (Unity titles supported)
- Process blacklist file (`process_blacklist.txt`) to hide common/system processes
- Injected process tagging in dropdown (`[Injected][pid] ...`)
- Single-instance controller protection
- Rolling status bar with auto-scroll and hooked-process exit notifications
- Elevated injector fallback when non-elevated injection fails
- Japanese README
### Changed
- WASAPI hooking/processing refinements and cleanup
- UI tweaks: status area height increased, tooltip updates for WASAPI mode
### Removed
- Removed unused hooks, including Fmod, XAudio2, Wwise.
- Removed several unused arg flags

## [1.1.0] - 2026-01-02
### Added
- Auto-hook feature with per-game config (auto_hook + process_bgm)
- Optional delayed auto-hook toggle (configurable delay)
- Hotkey to change 
- Speed hotkeys and `--speed` CLI argument
- UI localization with language dropdown, including Chinese, English, and Japanese
- Tooltips for labels and controls (including hotkey help)
### Changed
- UI layout updates and dropdown selection behavior refinements
- Dist staging structure changed for clarity

## [1.0.1] - 2025-12-29
### Fixed
- Passthrough empty audio buffer
- Fixed frequency changed by game: now check (and set) frequency for every sound buffer
- Fixed artifacts in streamed audios
### Changed
- Updated guide
### Added
- `--search` argument
### Other
- Cleanup

## [1.0.0] - 2025-12-14
### Changed
- Restructured
- Minor fixes
- Detailed readme
- Handles more cases

## [0.1.0] - 2025-12-10
### Added
- Initial release
