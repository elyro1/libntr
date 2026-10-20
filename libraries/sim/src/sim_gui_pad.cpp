#include "simulator/sim.h"
#include "simulator/sim_gui.hpp"

#include "simulator/imgui/imgui.hpp"
#include "simulator/config/sim_config.h"

#include "gui_internal.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <SDL2/SDL.h>

namespace SIM::GUI {

static constexpr ImVec2 BtnSize = {50, 20};

static SIM_config_type * sConfig;
static bool sConfigChanged = false;

static void SetKeyConfig(int& config) {
    SDL_Event Event;
    while(true) {
        while( SDL_PollEvent(&Event)) {
            if(Event.type == SDL_KEYDOWN)
            {
                config = Event.key.keysym.sym;
                return;
            }
        }
    }
}

static int *sPendingJoystickBinding = nullptr;

// Capture through the normal event loop so the window stays responsive and
// keyboard release / focus / device removal events are never swallowed.
bool AppPadProcessEvent(SDL_Event *event, bool active) {
    if (!active || (event->type == SDL_WINDOWEVENT &&
                    event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
        sPendingJoystickBinding = nullptr;
    }
    if (!sPendingJoystickBinding) return false;
    if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE) {
        sPendingJoystickBinding = nullptr;
        return true;
    }
    int binding = SIM_CONFIG_JOY_INVALID_KEY;
    if (event->type == SDL_JOYBUTTONDOWN) {
        binding = event->jbutton.button;
    } else if (event->type == SDL_JOYHATMOTION && event->jhat.hat == 0) {
        const int value = event->jhat.value;
        if (value == SDL_HAT_UP || value == SDL_HAT_DOWN ||
            value == SDL_HAT_LEFT || value == SDL_HAT_RIGHT) {
            binding = value | SIM_CONFIG_JOY_HAT_MASK;
        }
    } else if (event->type == SDL_JOYAXISMOTION) {
        // Capture requires a deliberate tilt even with a zero play deadzone.
        const int threshold = std::max(SIM_CONFIG_JOY_DEFAULT_DEADZONE,
                                      sConfig->padSettings.joyAxisDeadzone);
        if (event->jaxis.value > threshold || event->jaxis.value < -threshold) {
            binding = event->jaxis.axis | (event->jaxis.value > 0
                ? SIM_CONFIG_JOY_AXIS_PLUS_MASK : SIM_CONFIG_JOY_AXIS_MINUS_MASK);
        }
    }
    if (binding == SIM_CONFIG_JOY_INVALID_KEY) return false;
    *sPendingJoystickBinding = binding;
    sPendingJoystickBinding = nullptr;
    sConfigChanged = true;
    return true;
}

static std::string GetJoystickKeyName(int key) {
    if (key == SIM_CONFIG_JOY_INVALID_KEY) return "None";
    std::string ret = "";
    if(key & SIM_CONFIG_JOY_HAT_MASK) {
        ret = std::format("Hat {}", key & ~(SIM_CONFIG_JOY_HAT_MASK));
    } else if(key & SIM_CONFIG_JOY_AXIS_MINUS_MASK) {
        ret = std::format("Axis {}-", key & ~(SIM_CONFIG_JOY_AXIS_MINUS_MASK));
    } else if(key & SIM_CONFIG_JOY_AXIS_PLUS_MASK){
        ret = std::format("Axis {}+", key & ~(SIM_CONFIG_JOY_AXIS_PLUS_MASK));
    } else {
        ret = std::format("{}", key);
    }

    return ret;
}

static void KeySettingButton(const char * keyName, int btnId, int& key) {
    ImGui::Text(keyName);
    ImGui::SameLine(0.0f, 1.0f);
    ImGui::PushID(btnId);
    if(ImGui::Button(SDL_GetKeyName(key), BtnSize)) {
        SetKeyConfig(key);
        sConfigChanged = true;
    }
    ImGui::PopID();
}

static void JoystickBindingButton(int btnId, int& key, bool allowClear = false) {
    ImGui::PushID(btnId);
    auto buttonName = sPendingJoystickBinding == &key ? std::string("Press...") : GetJoystickKeyName(key);
    if (ImGui::Button(buttonName.c_str(), ImVec2(80, 20))) {
        sPendingJoystickBinding = &key;
    }
    if (allowClear) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            if (sPendingJoystickBinding == &key) sPendingJoystickBinding = nullptr;
            key = SIM_CONFIG_JOY_INVALID_KEY;
            sConfigChanged = true;
        }
    }
    ImGui::PopID();
}

static void JoystickKeySettingButton(const char * keyName, int btnId, int& key) {
    ImGui::TextUnformatted(keyName);
    ImGui::SameLine(0.0f, 1.0f);
    JoystickBindingButton(btnId, key);
}

void AppPadInit() {
    sPendingJoystickBinding = nullptr;
    sConfig = SIM_GetConfigPtr();
}

void AppPadMain(bool *openState) {
    ImGui::Begin("Input", openState);

    ImGui::Text("Keyboard");
    ImGui::Columns(2, "padKeyboard", true);

    KeySettingButton("A   ", 1, sConfig->padSettings.aKey);
    KeySettingButton("B   ", 2, sConfig->padSettings.bKey);
    KeySettingButton("X   ", 3, sConfig->padSettings.xKey);
    KeySettingButton("Y   ", 4, sConfig->padSettings.yKey);
    KeySettingButton("L   ", 5, sConfig->padSettings.lKey);
    KeySettingButton("R   ", 6, sConfig->padSettings.rKey);
    KeySettingButton("GUI ", 25, sConfig->padSettings.guiKey);
    KeySettingButton("Speedup ", 26, sConfig->padSettings.fastForwardKey);
    KeySettingButton("Fullscreen ", 27, sConfig->padSettings.fullScreenKey);

    ImGui::NextColumn();

    KeySettingButton("Up     ", 7,  sConfig->padSettings.upKey);
    KeySettingButton("Down   ", 8,  sConfig->padSettings.downKey);
    KeySettingButton("Left   ", 9,  sConfig->padSettings.leftKey);
    KeySettingButton("Right  ", 10, sConfig->padSettings.rightKey);
    KeySettingButton("Start  ", 11, sConfig->padSettings.startKey);
    KeySettingButton("Select ", 12, sConfig->padSettings.selectKey);

    ImGui::Columns(1, "pad2", false);
    ImGui::Separator();
    ImGui::Text("Gamepad");
    ImGui::Columns(2, "padJoystick", true);

    JoystickKeySettingButton("A ", 13, sConfig->padSettings.aJoyKey);
    JoystickKeySettingButton("B ", 14, sConfig->padSettings.bJoyKey);
    JoystickKeySettingButton("X ", 15, sConfig->padSettings.xJoyKey);
    JoystickKeySettingButton("Y ", 16, sConfig->padSettings.yJoyKey);
    JoystickKeySettingButton("L ", 17, sConfig->padSettings.lJoyKey);
    JoystickKeySettingButton("R ", 18, sConfig->padSettings.rJoyKey);

    ImGui::NextColumn();

    JoystickKeySettingButton("Start  ", 23, sConfig->padSettings.startJoyKey);
    JoystickKeySettingButton("Select ", 24, sConfig->padSettings.selectJoyKey);

    ImGui::Columns(1);
    ImGui::Separator();
    ImGui::TextUnformatted("Directions: D-pad and left stick work everywhere");
    if (ImGui::BeginTable("directions", 3, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Direction");
        ImGui::TableSetupColumn("Primary");
        ImGui::TableSetupColumn("Secondary");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Up");
        ImGui::TableNextColumn(); JoystickBindingButton(19, sConfig->padSettings.upJoyKey);
        ImGui::TableNextColumn(); JoystickBindingButton(29, sConfig->padSettings.upJoyKeySecondary, true);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Down");
        ImGui::TableNextColumn(); JoystickBindingButton(20, sConfig->padSettings.downJoyKey);
        ImGui::TableNextColumn(); JoystickBindingButton(30, sConfig->padSettings.downJoyKeySecondary, true);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Left");
        ImGui::TableNextColumn(); JoystickBindingButton(21, sConfig->padSettings.leftJoyKey);
        ImGui::TableNextColumn(); JoystickBindingButton(31, sConfig->padSettings.leftJoyKeySecondary, true);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Right");
        ImGui::TableNextColumn(); JoystickBindingButton(22, sConfig->padSettings.rightJoyKey);
        ImGui::TableNextColumn(); JoystickBindingButton(32, sConfig->padSettings.rightJoyKeySecondary, true);
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Click a binding, then press a direction. Escape cancels. Use Secondary for the left stick.");

    float deadzonePercent = sConfig->padSettings.joyAxisDeadzone * 100.0f / 32767.0f;
    if (ImGui::SliderFloat("Axis deadzone", &deadzonePercent, 0.0f, 95.0f, "%.0f%%",
                           ImGuiSliderFlags_AlwaysClamp)) {
        sConfig->padSettings.joyAxisDeadzone = (int)(deadzonePercent * 32767.0f / 100.0f);
        sConfigChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##deadzone")) {
        sConfig->padSettings.joyAxisDeadzone = SIM_CONFIG_JOY_DEFAULT_DEADZONE;
        sConfigChanged = true;
    }
    ImGui::TextWrapped("Applies to all mapped controller axes. 0%% disables filtering; increase it if the stick drifts.");

    ImGui::End();

    if(sConfigChanged) {
        SIM_Config_SaveConfigFile(sConfig);
        sConfigChanged = false;
    }
}

}