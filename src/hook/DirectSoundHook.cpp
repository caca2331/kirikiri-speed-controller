#include "DirectSoundHook.h"

namespace krkrspeed {

DirectSoundHook &DirectSoundHook::instance() {
    static DirectSoundHook hook;
    return hook;
}

void DirectSoundHook::initialize() { hookEntryPoints(); }

void DirectSoundHook::hookEntryPoints() {
    // Placeholder for MinHook wiring for DirectSoundCreate8 and buffer methods.
}

} // namespace krkrspeed
