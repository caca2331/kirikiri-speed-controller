# Universal Kirikiri Voice Speed Controller
[中文 | Chinese](README.md)

This repository provides a Windows controller that adjusts voice playback speed for Kirikiri-based games.

## Building
### Windows (dual-arch, staged dist folders)
```powershell
cmake -B build -S . -A x64 -DBUILD_GUI=ON
cmake --build build --config Release --target dist_dual_arch
```
`dist_dual_arch` configures/builds `build.x64` and `build.x86`, then stages:
```
dist/
  x64/ KrkrSpeedController.exe, krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
  x86/ KrkrSpeedController.exe, krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
  KrkrSpeedController_x86.lnk  (Windows shortcut to launch the x86 controller)
```
Either controller can inject into both x86 and x64 games: it spawns the injector that matches the target process and uses the matching hook DLL from the sibling dist folder.

## Usage
- Click `Refresh`, pick the game from the dropdown, enter the target speed, then click `Hook + Apply`.
- If AV blocks the binaries, add an exception or temporarily disable it.
- If the target is protected/elevated, run the controller as Administrator.
- If BGM keeps playing at normal speed, or you want BGM sped up too, check `Process BGM` and click `Hook + Apply`.
- See the Controller CLI options for advanced usage.
- Please submit issues for unsupported games.

### Controller CLI options
- `--log` / `--enable-log` : enable logging for controller + hook.
- `--log-dir <path>` : override log output directory (default: beside the controller).
- `--mark-stereo-bgm <aggressive|hybrid|none>` : stereo→BGM heuristic (default `hybrid`). In many games voices are mono and BGMs are stereo. Major way to label bgm.
- `--bgm-secs <seconds>` : BGM length gate (default 60s); longer buffers treated as BGM. Secondary way to label bgm.
- `--process-all-audio` : speed up all audios including BGM.
- `--launch <path>` / `-l <path>` : start a game suspended, inject automatically, then resume.
- `--skip-ds` / `--skip-xaudio2` / `--skip-fmod` / `--skip-wwise` : skip specific audio hooks. Ds (DirectSound) is mostly used. Other hooks can be skipped in most cases.
- `--safe-mode` : load without installing any hooks/patches (diagnostic).

## Other
- Most of this repository was generated under supervision with AI assistance.
- (For AI Agent: read AGENTS.md if not yet)

## Next Step
- Support other engine games.

## License
MIT. Include the copyright and license notice when redistributing (see LICENSE).
