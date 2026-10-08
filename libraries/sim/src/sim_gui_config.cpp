#include "simulator/sim.h"
#include "simulator/sim_gui.hpp"

#include "simulator/imgui/imgui.hpp"
#include "simulator/config/sim_config.h"

#include "gui_internal.hpp"

#include <array>
#include <SDL2/SDL.h>

namespace SIM::GUI {

static constexpr ImVec2 s_btnSize = {50, 20};

static SIM_config_type * sConfig;

static constexpr const char * ScreenLayoutStrings[] = {
    "Vertical",
    "Horizontal",
    "Large Screen",
    "Top Screen / Widescreen"
};

static constexpr const char * VblankStrings[] = {
    "Off",
    "Every V-Blank",
    "Every second V-Blank"
};

void AppConfigInit() {
    sConfig = SIM_GetConfigPtr();
}

void AppConfigMain(bool *openState) {
    bool configChanged = false;
    int screenLayout = sConfig->screenLayout;
    bool swapScreens = sConfig->swapScreens;
    int vSyncInterval = sConfig->vsyncInterval;
    bool frameLimit = sConfig->capFrameRate;
    float fastForwardMultiplier = sConfig->fastForwardMultiplier;
    
    ImGui::Begin("Config", openState);

    if(ImGui::Combo("Layout", &screenLayout, ScreenLayoutStrings, 4, 4)) {
        configChanged = true;
    }
    if (screenLayout == SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY) {
        ImGui::TextWrapped("Overworld expands up to 21:9. Bottom-screen controls are hidden in this prototype.");
    }
    if(screenLayout != SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY && ImGui::Checkbox("Swap Screens", &swapScreens)) {
        configChanged = true;
    }
    if(ImGui::Combo("VSync", &vSyncInterval, VblankStrings, 3, 3)) {
        configChanged = true;
    }
    if(ImGui::Checkbox("Cap framerate", &frameLimit)) {
        configChanged = true;
    }
    if (ImGui::SliderFloat("Fast-forward speed", &fastForwardMultiplier, 1.0f, 10.0f, "%.2fx")) {
        configChanged = true;
    }
    ImGui::TextWrapped("Hold %s to speed up the game and sound effects. Background music keeps its normal speed.",
        SDL_GetKeyName(sConfig->padSettings.fastForwardKey));

    ImGui::End();

    if(configChanged) {
        sConfig->screenLayout = static_cast<SIM_config_screen_layout_type>(screenLayout);
        sConfig->swapScreens = swapScreens;
        sConfig->vsyncInterval = vSyncInterval;
        if(vSyncInterval != SDL_GL_GetSwapInterval()) {
            SDL_GL_SetSwapInterval(vSyncInterval);
        }
        sConfig->capFrameRate = frameLimit;
        sConfig->fastForwardMultiplier = SIM_Config_ValidateFastForwardMultiplier(fastForwardMultiplier);

        SIM_Config_SaveConfigFile(sConfig);
    }
}

}
