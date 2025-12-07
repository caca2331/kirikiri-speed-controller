# Universal Kirikiri Voice Speed Controller (Skeleton)

This repository contains an early scaffold for a Kirikiri voice speed controller based on the provided technical specification. The Windows-specific hook targets are not built on non-Windows hosts, but the DSP core and tests can still be compiled for smoke testing.

## File-by-file overview
- `CMakeLists.txt`: Configures the shared DSP library, optional Windows-only hook/GUI targets, and the DSP smoke test;
  toggles SoundTouch support via `USE_SOUNDTOUCH` and guards GUI/DLL builds behind platform checks.
- `src/common/DspPipeline.h` / `src/common/DspPipeline.cpp`: Declares and implements the tempo-adjustment pipeline,
  including pluggable DSP backends, buffer management helpers, and WSOLA-aligned defaults for speech.
- `src/common/VoiceContext.h`: Defines the `VoiceContext` structure that tracks user speed, engine frequency ratio, speech
  classification flags, and queued buffer metadata for safe lifetime handling.
- `src/hook/XAudio2Hook.h` / `src/hook/XAudio2Hook.cpp`: Placeholder hook surface for XAudio2 with version detection stubs,
  vtable scanning helpers, and submit-voice interception points awaiting full MinHook wiring.
- `src/hook/DirectSoundHook.h` / `src/hook/DirectSoundHook.cpp`: Skeleton for DirectSound8 interception, outlining shadow-
  buffer handling and hook registration points for future implementation.
- `src/hook/dllmain.cpp`: DLL entry that initializes logging and kicks off hook setup on load; currently stubbed to avoid
  running on unsupported hosts during development.
- `src/gui/main.cpp`: Minimal controller stub to be expanded into a process picker, injector, and live speed control GUI
  once Windows builds are enabled.
- `tests/dsp_smoke.cpp`: Generates synthetic sine data to validate tempo changes across multiple ratios and channel counts,
  ensuring the DSP pipeline is wired correctly in isolation.

## Building
Use CMake to generate a Visual Studio solution. SoundTouch integration is disabled by default to keep the scaffold self-contained; enable with `-DUSE_SOUNDTOUCH=ON` after providing the dependency.

### Windows (full targets)
```powershell
cmake -B build -S . -DUSE_SOUNDTOUCH=ON -DBUILD_GUI=ON
cmake --build build --config Release
```

### Non-Windows smoke tests only
```bash
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_GUI=OFF
cmake --build build
ctest --test-dir build -V
```

## Next Steps
- Wire MinHook to the XAudio2 and DirectSound entry points.
- Replace the DSP fallback with the tuned SoundTouch configuration (sequence/overlap/seek window).
- Implement voice classification heuristics and GUI controls for real-time speed changes.

---

# 通用Kirikiri语音变速控制器（骨架版）

本仓库提供一个按照技术规范搭建的Kirikiri语音变速控制器脚手架。在非Windows环境下不会编译Windows特定的Hook和GUI目标，但仍可编译DSP核心与冒烟测试。

## 文件逐一说明
- `CMakeLists.txt`：配置共享DSP库、可选的仅限Windows的Hook/GUI目标以及DSP冒烟测试；通过`USE_SOUNDTOUCH`切换
  SoundTouch支持，并用平台判断保护GUI/DLL构建。
- `src/common/DspPipeline.h` / `src/common/DspPipeline.cpp`：声明并实现变速处理管线，包括可插拔的DSP后端、缓冲区管理
  工具，以及针对语音的WSOLA默认参数。
- `src/common/VoiceContext.h`：定义`VoiceContext`结构，用于跟踪用户速度、引擎频率比、语音分类标志以及缓冲区元数据
  队列以保证生命周期安全。
- `src/hook/XAudio2Hook.h` / `src/hook/XAudio2Hook.cpp`：XAudio2拦截的占位实现，包含版本检测骨架、虚表扫描辅助方法，
  以及等待与MinHook对接的提交拦截点。
- `src/hook/DirectSoundHook.h` / `src/hook/DirectSoundHook.cpp`：DirectSound8拦截的空架构，描述影子缓冲处理和未来的Hook注
  册入口。
- `src/hook/dllmain.cpp`：DLL入口，在加载时初始化日志并启动Hook设置；为避免在不支持的主机上运行，目前保持空操作。
- `src/gui/main.cpp`：最小化的控制器占位，将在启用Windows构建后扩展为进程选择器、注入器以及实时变速控制GUI。
- `tests/dsp_smoke.cpp`：生成合成正弦波数据，在多种倍速和声道数量下验证变速效果，确保DSP管线在隔离环境下运作
  正确。

## 构建方式
使用CMake生成Visual Studio解决方案。默认关闭SoundTouch以保持脚手架自包含；在准备好依赖后可通过`-DUSE_SOUNDTOUCH=ON`开启。

### Windows（完整目标）
```powershell
cmake -B build -S . -DUSE_SOUNDTOUCH=ON -DBUILD_GUI=ON
cmake --build build --config Release
```

### 非Windows（仅冒烟测试）
```bash
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_GUI=OFF
cmake --build build
ctest --test-dir build -V
```

## 下一步计划
- 将MinHook接入XAudio2和DirectSound入口。
- 用针对语音的SoundTouch参数（序列/重叠/搜索窗口）替换当前的后备方案。
- 实现语音分类启发式与GUI的实时速度调节。
