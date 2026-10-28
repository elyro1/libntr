#include "simulator/sim.h"
#include "simulator/sim_gui.hpp"
#include "simulator/imgui/imgui.hpp"
#include "gui_internal.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace SIM::GUI {
static SIM_config_type *sConfig;
static int *sPendingKeyboardBinding;
static int *sPendingJoystickBinding;

bool AppPadIsCapturing() { return sPendingKeyboardBinding || sPendingJoystickBinding; }

// Binding capture shares the main event loop: no spinning, frozen rendering,
// or swallowed focus/device/quit events.
bool AppPadProcessEvent(SDL_Event *event, bool active) {
    if (!active || (event && event->type == SDL_WINDOWEVENT &&
                    event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) ||
        (event && event->type == SDL_JOYDEVICEREMOVED)) {
        sPendingKeyboardBinding = nullptr;
        sPendingJoystickBinding = nullptr;
    }
    if (!event || !AppPadIsCapturing()) return false;
    if (event->type == SDL_KEYDOWN) {
        if (event->key.repeat) return true;
        if (event->key.keysym.sym == SDLK_ESCAPE) {
            sPendingKeyboardBinding = nullptr;
            sPendingJoystickBinding = nullptr;
        } else if (sPendingKeyboardBinding) {
            *sPendingKeyboardBinding = event->key.keysym.sym;
            sPendingKeyboardBinding = nullptr;
            SIM_Config_SaveConfigFile(sConfig);
        }
        return true;
    }
    if (!sPendingJoystickBinding) return false;
    int binding = SIM_CONFIG_JOY_INVALID_KEY;
    if (event->type == SDL_JOYBUTTONDOWN) {
        binding = event->jbutton.button;
    } else if (event->type == SDL_JOYHATMOTION && event->jhat.hat == 0) {
        const int value = event->jhat.value;
        if (value == SDL_HAT_UP || value == SDL_HAT_DOWN ||
            value == SDL_HAT_LEFT || value == SDL_HAT_RIGHT)
            binding = value | SIM_CONFIG_JOY_HAT_MASK;
    } else if (event->type == SDL_JOYAXISMOTION) {
        const int threshold = std::max(SIM_CONFIG_JOY_DEFAULT_DEADZONE,
                                      sConfig->padSettings.joyAxisDeadzone);
        if (event->jaxis.value > threshold || event->jaxis.value < -threshold)
            binding = event->jaxis.axis | (event->jaxis.value > 0
                ? SIM_CONFIG_JOY_AXIS_PLUS_MASK : SIM_CONFIG_JOY_AXIS_MINUS_MASK);
    }
    if (binding == SIM_CONFIG_JOY_INVALID_KEY) return false;
    *sPendingJoystickBinding = binding;
    sPendingJoystickBinding = nullptr;
    SIM_Config_SaveConfigFile(sConfig);
    return true;
}

static std::string JoystickName(int key) {
    if (key == SIM_CONFIG_JOY_INVALID_KEY) return "None";
    if (key & SIM_CONFIG_JOY_HAT_MASK)
        return std::format("Hat {}", key & ~SIM_CONFIG_JOY_HAT_MASK);
    if (key & SIM_CONFIG_JOY_AXIS_MINUS_MASK)
        return std::format("Axis {}-", key & ~SIM_CONFIG_JOY_AXIS_MINUS_MASK);
    if (key & SIM_CONFIG_JOY_AXIS_PLUS_MASK)
        return std::format("Axis {}+", key & ~SIM_CONFIG_JOY_AXIS_PLUS_MASK);
    return std::format("Button {}", key);
}

static void KeyboardRow(const char *label, int &key) {
    SettingsRow(label);
    ImGui::PushID(&key);
    if (ImGui::Button(sPendingKeyboardBinding == &key ? "Press a key..." : SDL_GetKeyName(key),
                      ImVec2(-1, 0))) {
        sPendingJoystickBinding = nullptr;
        sPendingKeyboardBinding = &key;
    }
    ImGui::PopID();
}

static void ControllerRow(const char *label, int &key, bool clear = false) {
    SettingsRow(label);
    ImGui::PushID(&key);
    const std::string name = sPendingJoystickBinding == &key ? "Press a button..." : JoystickName(key);
    const float clearWidth = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2;
    const float width = clear ? std::max(ImGui::GetFontSize() * 3,
        ImGui::GetContentRegionAvail().x - clearWidth - ImGui::GetStyle().ItemSpacing.x) : -1;
    if (ImGui::Button(name.c_str(), ImVec2(width, 0))) {
        sPendingKeyboardBinding = nullptr;
        sPendingJoystickBinding = &key;
    }
    if (clear) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            if (sPendingJoystickBinding == &key) sPendingJoystickBinding = nullptr;
            key = SIM_CONFIG_JOY_INVALID_KEY;
            SIM_Config_SaveConfigFile(sConfig);
        }
    }
    ImGui::PopID();
}

void AppPadInit() { sConfig = SIM_GetConfigPtr(); }

void AppPadContents() {
    static int activeDevice = 0;
    ImGui::TextWrapped("Select a binding, then press a key or controller input. Escape cancels.");
    if (AppPadIsCapturing())
        ImGui::TextWrapped("Waiting for input... Escape cancels without changing the binding.");
    if (!ImGui::BeginTabBar("InputDevice")) return;
    if (ImGui::BeginTabItem("Keyboard")) {
        if (activeDevice != 0) AppPadProcessEvent(nullptr, false);
        activeDevice = 0;
        if (BeginSettings("KeyboardBindings")) {
            KeyboardRow("A - Confirm / interact", sConfig->padSettings.aKey);
            KeyboardRow("B - Cancel / run", sConfig->padSettings.bKey);
            KeyboardRow("X - In-game menu", sConfig->padSettings.xKey);
            KeyboardRow("Y - Registered item", sConfig->padSettings.yKey);
            KeyboardRow("L - Previous app", sConfig->padSettings.lKey);
            KeyboardRow("R - Next app", sConfig->padSettings.rKey);
            KeyboardRow("Up", sConfig->padSettings.upKey);
            KeyboardRow("Down", sConfig->padSettings.downKey);
            KeyboardRow("Left", sConfig->padSettings.leftKey);
            KeyboardRow("Right", sConfig->padSettings.rightKey);
            KeyboardRow("Start", sConfig->padSettings.startKey);
            KeyboardRow("Select", sConfig->padSettings.selectKey);
            KeyboardRow("Settings menu", sConfig->padSettings.guiKey);
            KeyboardRow("Fast-forward (hold)", sConfig->padSettings.fastForwardKey);
            KeyboardRow("Fullscreen", sConfig->padSettings.fullScreenKey);
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Controller")) {
        if (activeDevice != 1) AppPadProcessEvent(nullptr, false);
        activeDevice = 1;
        if (BeginSettings("ControllerBindings")) {
            ControllerRow("A - Confirm / interact", sConfig->padSettings.aJoyKey);
            ControllerRow("B - Cancel / run", sConfig->padSettings.bJoyKey);
            ControllerRow("X - In-game menu", sConfig->padSettings.xJoyKey);
            ControllerRow("Y - Registered item", sConfig->padSettings.yJoyKey);
            ControllerRow("L - Previous app", sConfig->padSettings.lJoyKey);
            ControllerRow("R - Next app", sConfig->padSettings.rJoyKey);
            ControllerRow("Start", sConfig->padSettings.startJoyKey);
            ControllerRow("Select", sConfig->padSettings.selectJoyKey);
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Movement");
        ImGui::TextWrapped("Use primary for the D-pad and secondary for the left stick.");
        if (BeginSettings("DirectionBindings")) {
            ControllerRow("Up - primary", sConfig->padSettings.upJoyKey);
            ControllerRow("Up - secondary", sConfig->padSettings.upJoyKeySecondary, true);
            ControllerRow("Down - primary", sConfig->padSettings.downJoyKey);
            ControllerRow("Down - secondary", sConfig->padSettings.downJoyKeySecondary, true);
            ControllerRow("Left - primary", sConfig->padSettings.leftJoyKey);
            ControllerRow("Left - secondary", sConfig->padSettings.leftJoyKeySecondary, true);
            ControllerRow("Right - primary", sConfig->padSettings.rightJoyKey);
            ControllerRow("Right - secondary", sConfig->padSettings.rightJoyKeySecondary, true);
            ImGui::EndTable();
        }
        float deadzone = sConfig->padSettings.joyAxisDeadzone * 100.0f / 32767.0f;
        if (BeginSettings("Deadzone")) {
            SettingsRow("Axis deadzone", "0% disables filtering. Increase to prevent stick drift.");
            if (ImGui::SliderFloat("##Deadzone", &deadzone, 0, 95, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                sConfig->padSettings.joyAxisDeadzone = (int)(deadzone * 32767.0f / 100.0f);
                SIM_Config_SaveConfigFile(sConfig);
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Reset deadzone")) {
            sConfig->padSettings.joyAxisDeadzone = SIM_CONFIG_JOY_DEFAULT_DEADZONE;
            SIM_Config_SaveConfigFile(sConfig);
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}
}
