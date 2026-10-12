# Universal Kirikiri Voice Speed Controller
[中文](README.md) | [日本語](README_ja.md)

This repository provides a Windows controller that adjusts voice playback speed for games.
- For DirectSound titles (e.g., Kirikiri engine games):
  - Speed range is roughly 0.5x–~2.3x
  - Usually voice can be sped up without affecting BGM.
- For games that call WASAPI directly (e.g., most Unity titles):
  - Speed range is 1x–10x
  - Only global speedup is available.

## Building
### Windows (dual-arch, staged dist folders)
```powershell
cmake -B build -S . -A x64 -DBUILD_GUI=ON
cmake --build build --config Release --target dist_dual_arch
```
`dist_dual_arch` configures/builds `build.x64` and `build.x86`, then stages:
```
dist/
  KrkrSpeedController/
    KrkrSpeedController.exe, SoundTouch.dll
    x86/ krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
    x64/ krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
```
The controller can inject into both x86 and x64 games: it spawns the injector that matches the target process and uses the matching hook DLL from the arch subfolder.

## Usage
### Basic
- Launch `KrkrSpeedController.exe`.
- Pick the game from the dropdown, enter the target speed, then click `Hook`.
- If AV blocks the binaries, add an exception or temporarily disable it.
- If the target is protected/elevated, run the controller as Administrator.
- Please submit issues for unsupported games.

### Advanced
- Global hotkeys:
  - `Alt + '`: toggle speed on/off.
  - `Alt + ]`: speed up 0.1x (if off, turns on and sets 1.1x).
  - `Alt + [`: speed down 0.1x (if off, turns on and sets 0.9x).
  - Hotkeys and the speed box apply to every hooked game, regardless of the dropdown selection.
- If voices fail to speed up, or you want BGM sped up too, check `Process BGM`.
- Auto-inject: select a game from the dropdown and check `Auto-Hook This App`. When the game is detected running, it auto-injects; unchecking removes it from the config.
- Keep the controller running in the background; after the first injection, enable auto-hook so future launches are injected and sped up automatically.
- Edit `process_blacklist.txt` beside the controller to hide processes from the process selection dropdown.
- Can be used together with Cheat Engine speed hacks.

### Controller CLI options
- `--log` : enable logging for controller + hook. Logs are written beside the controller: `krkr_controller.log`, plus one `krkr_hook_<PID>.log` per hooked game.
- `--speed <value>` : set the initial speed on startup (default 1.5).
- `--mark-stereo-bgm <aggressive|hybrid|none>` : DirectSound only. Stereo→BGM heuristic (default `hybrid`). In many games voices are mono and BGMs are stereo. `aggressive`: always mark stereo as BGM. `none`: never treat stereo as a BGM signal. Primary BGM labeling method.
- `--bgm-secs <seconds>` : BGM length gate (default 60s); longer buffers treated as BGM. Secondary way to label bgm.
- `--launch <path>` / `-l <path>` : start a game suspended, inject automatically, then resume.
- `--search <name>` : on startup, find visible processes whose name contains this substring; if multiple match, pick the one with the shortest name and auto-inject. If none match, the controller starts normally and waits for manual selection.

## Others
- Most of this repository was generated under supervision with AI assistance.
- (For AI Agent: read AGENTS.md if not yet)

## Next Step
- None.

## License
MIT. Include the copyright and license notice when redistributing (see LICENSE).

## Stargazers over time
[![Stargazers over time](https://starchart.cc/caca2331/kirikiri-speed-controller.svg?variant=adaptive)](https://starchart.cc/caca2331/kirikiri-speed-controller)