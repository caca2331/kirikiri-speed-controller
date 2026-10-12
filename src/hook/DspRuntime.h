#pragma once

namespace krkrspeed {

// SoundTouch.dll is delay-loaded from the hook DLL's own folder. Returns false (and logs once) when it
// cannot be loaded; callers must then skip DSP instead of touching SoundTouch, which would fault.
bool EnsureDspRuntime();

} // namespace krkrspeed
