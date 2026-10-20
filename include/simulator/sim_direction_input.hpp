#ifndef SIM_DIRECTION_INPUT_HPP
#define SIM_DIRECTION_INPUT_HPP

#include <SDL2/SDL.h>
#include <simulator/config/sim_config.h>

namespace SIM::Input {

// DS direction bits. Keep source state separate until writing the PAD register.
constexpr unsigned DirectionMask = 0x00f0;

inline bool JoystickBindingHeld(SDL_Joystick *joystick, int binding, int deadzone) {
    if (!joystick || !SDL_JoystickGetAttached(joystick) || binding < 0) return false;
    if (binding & SIM_CONFIG_JOY_HAT_MASK) {
        const int hat = binding & ~SIM_CONFIG_JOY_HAT_MASK;
        return SDL_JoystickNumHats(joystick) > 0 && hat &&
            (SDL_JoystickGetHat(joystick, 0) & hat) == hat;
    }
    const int axisMasks = SIM_CONFIG_JOY_AXIS_PLUS_MASK | SIM_CONFIG_JOY_AXIS_MINUS_MASK;
    if (binding & axisMasks) {
        const int axis = binding & ~axisMasks;
        if (axis >= SDL_JoystickNumAxes(joystick)) return false;
        const int value = SDL_JoystickGetAxis(joystick, axis);
        return binding & SIM_CONFIG_JOY_AXIS_PLUS_MASK ? value > deadzone : value < -deadzone;
    }
    return binding < SDL_JoystickNumButtons(joystick) && SDL_JoystickGetButton(joystick, binding);
}

inline unsigned JoystickDirections(SDL_Joystick *joystick, const SIM_config_pad_settings_type &pad) {
    const int primary[] = {pad.rightJoyKey, pad.leftJoyKey, pad.upJoyKey, pad.downJoyKey};
    const int secondary[] = {pad.rightJoyKeySecondary, pad.leftJoyKeySecondary,
                             pad.upJoyKeySecondary, pad.downJoyKeySecondary};
    unsigned held = 0;
    for (int i = 0; i < 4; ++i) {
        if (JoystickBindingHeld(joystick, primary[i], pad.joyAxisDeadzone) ||
            JoystickBindingHeld(joystick, secondary[i], pad.joyAxisDeadzone)) {
            held |= 0x10u << i;
        }
    }
    return held;
}

inline unsigned ApplyDirections(unsigned padRegister, unsigned keyboardHeld, unsigned joystickHeld) {
    return (padRegister | DirectionMask) & ~((keyboardHeld | joystickHeld) & DirectionMask);
}

} // namespace SIM::Input

#endif
