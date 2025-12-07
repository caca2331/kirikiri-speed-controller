#include "../hook/XAudio2Hook.h"
#include <iostream>

int main() {
    std::cout << "KrkrSpeedController stub running.\n";
    std::cout << "Use this utility to inject krkr_speed_hook.dll and control tempo." << std::endl;
    krkrspeed::XAudio2Hook::instance().setUserSpeed(1.0f);
    std::cout << "Default speed set to 1.0x" << std::endl;
    return 0;
}
