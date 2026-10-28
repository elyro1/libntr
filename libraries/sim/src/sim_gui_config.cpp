#include "simulator/sim.h"
#include "simulator/sim_audio.h"
#include "simulator/sim_gui.hpp"
#include "simulator/imgui/imgui.hpp"
#include "gui_internal.hpp"

namespace SIM::GUI {
void DisplayContents() {
    auto *config = SIM_GetConfigPtr();
    bool changed = false;
    if (!BeginSettings("DisplaySettings")) return;
    bool fullscreen = config->fullScreen;
    SettingsRow("Window mode");
    int mode = fullscreen ? 0 : 1;
    if (ImGui::Combo("##WindowMode", &mode, "Fullscreen\0Windowed\0")) {
        fullscreen = mode == 0;
        if (SDL_SetWindowFullscreen(SIM_GetSDLWindow(), fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) {
            config->fullScreen = fullscreen;
            changed = true;
        }
    }
    SettingsRow("Screen layout", "Single screen expands the overworld up to 21:9.");
    int layout = config->screenLayout;
    if (ImGui::Combo("##Layout", &layout, "Vertical\0Horizontal\0Large screen\0Single screen / Widescreen\0")) {
        config->screenLayout = static_cast<SIM_config_screen_layout_type>(layout);
        changed = true;
    }
    if (layout != SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY) {
        SettingsRow("Swap screens");
        bool swap = config->swapScreens;
        if (ImGui::Checkbox("##Swap", &swap)) { config->swapScreens = swap; changed = true; }
    }
    SettingsRow("Menu scale", "Auto follows the display. Override if text is too small.");
    const char *scales[] = {"Auto", "100%", "125%", "150%", "175%", "200%"};
    int scale = config->menuScalePercent ? (config->menuScalePercent - 100) / 25 + 1 : 0;
    if (ImGui::Combo("##MenuScale", &scale, scales, 6)) {
        config->menuScalePercent = scale ? 100 + (scale - 1) * 25 : 0;
        changed = true;
    }
    SettingsRow("Internal resolution", "Game sharpness; independent of menu size.");
    int resolution = config->internalResolutionScale;
    if (ImGui::SliderInt("##Resolution", &resolution, 1, 20, "%dx NDS"))
        SIM_SetInternalResolutionAfterRender(resolution);
    SettingsRow("VSync");
    int vsync = config->vsyncInterval;
    if (ImGui::Combo("##VSync", &vsync, "Off\0Every refresh\0Every second refresh\0")) {
        config->vsyncInterval = vsync;
        SDL_GL_SetSwapInterval(vsync);
        changed = true;
    }
    ImGui::EndTable();
    if (changed) SIM_Config_SaveConfigFile(config);
}

void AudioContents() {
    auto *config = SIM_GetConfigPtr();
    if (!BeginSettings("AudioSettings")) return;
    SettingsRow("Master volume");
    int volume = config->masterVolume * 100 / 127;
    if (ImGui::SliderInt("##Volume", &volume, 0, 100, "%d%%")) {
        config->masterVolume = (volume * 127 + 50) / 100;
        SIM_Audio_SetMasterVolume(config->masterVolume);
        SIM_Config_SaveConfigFile(config);
    }
    ImGui::EndTable();
}

void SpeedContents() {
    auto *config = SIM_GetConfigPtr();
    bool changed = false;
    ImGui::SeparatorText("Playback");
    if (!BeginSettings("SpeedSettings")) return;
    SettingsRow("Cap frame rate");
    bool cap = config->capFrameRate;
    if (ImGui::Checkbox("##Cap", &cap)) { config->capFrameRate = cap; changed = true; }
    SettingsRow("Fast-forward speed", "Background music keeps its normal speed.");
    float speed = config->fastForwardMultiplier;
    if (ImGui::SliderFloat("##FastForward", &speed, 1, 10, "%.2fx")) {
        config->fastForwardMultiplier = SIM_Config_ValidateFastForwardMultiplier(speed);
        changed = true;
    }
    ImGui::EndTable();
    ImGui::TextWrapped("Hold %s to fast-forward during gameplay.",
        SDL_GetKeyName(config->padSettings.fastForwardKey));
    if (changed) SIM_Config_SaveConfigFile(config);
}
}
