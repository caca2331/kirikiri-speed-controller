#pragma once

#include <windows.h>

namespace krkrspeed {

class DirectSoundHook {
public:
    static DirectSoundHook &instance();
    void initialize();

private:
    DirectSoundHook() = default;
    void hookEntryPoints();
};

} // namespace krkrspeed
