#include "simulator/sim.h"
#include "simulator/sim_gui.hpp"
#include "simulator/sim_net.h"
#include "simulator/imgui/imgui.hpp"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "gui_internal.hpp"
#include "ImGuiFileDialog.h"

#include <algorithm>
#include <cmath>

namespace SIM::GUI {
static SDL_Window *sWindow;
static bool sEnabled;
static bool sManualPause;
static bool sInputReset;
static bool sFocusPanel;
static bool sShowImGuiDemo;
static bool sSuppressedKeys[SDL_NUM_SCANCODES];
static bool sStackSettings;
static Page sPage = Page::Display;
static ImGuiStyle sBaseStyle;
static float sScale = 1.0f;
static std::function<void(std::string)> sFileDialogCallback;

static const char *const sPageNames[] = {
    "Display", "Graphics", "Audio", "Controls", "Gameplay", "Game speed",
    "Cheats & tools", "Multiplayer", "Developer"
};

void __attribute__((weak)) ProjectPage(Page page) {
    if (page == Page::Graphics || page == Page::Tools || page == Page::Gameplay)
        ImGui::TextWrapped("This application does not provide additional settings here.");
}
bool __attribute__((weak)) ProjectBack(Page) { return false; }

static bool MultiplayerActive() {
    const auto state = SIM_Net_GetCurrentState();
    return state == SIM_NET_STATE_HOST || state == SIM_NET_STATE_CLIENT;
}

bool IsEnabled() { return sEnabled; }

bool ConsumeInputReset() {
    const bool reset = sInputReset;
    sInputReset = false;
    return reset;
}

void SelectPage(Page page) {
    if (page < Page::Display || page >= Page::Count || page == sPage) return;
    sPage = page;
    AppPadProcessEvent(nullptr, false);
}

static void SetEnabled(bool enabled) {
    if (enabled == sEnabled) return;
    sEnabled = enabled;
    sInputReset = true;
    sFocusPanel = enabled;
    AppPadProcessEvent(nullptr, false);
    // Do not turn a held navigation key into a gameplay press after closing.
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    for (int i = 0; i < SDL_NUM_SCANCODES; ++i)
        sSuppressedKeys[i] = sSuppressedKeys[i] || keys[i];
    if (!enabled) {
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearEventsQueue();
        sShowImGuiDemo = false;
        ImGuiFileDialog::Instance()->Close();
        sFileDialogCallback = nullptr;
    }
}

void Toggle() { SetEnabled(!sEnabled); }
void Enable() { SetEnabled(true); }

bool IsGameLogicPaused() {
    // Network peers must keep progressing even if the local menu is open.
    return sManualPause || (sEnabled && SIM_GetConfigPtr()->pauseWhileMenuOpen && !MultiplayerActive());
}

bool BeginSettings(const char *id) {
    sStackSettings = ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 28.0f;
    if (!ImGui::BeginTable(id, sStackSettings ? 1 : 2, ImGuiTableFlags_SizingStretchProp))
        return false;
    if (!sStackSettings) {
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 1.15f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    }
    return true;
}

void SettingsRow(const char *label, const char *description) {
    ImGui::TableNextRow(0, ImGui::GetFrameHeightWithSpacing());
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", label);
    if (description) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", description);
        ImGui::PopStyleColor();
    }
    if (sStackSettings) ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
}

void Init(SDL_Window *window, SDL_GLContext context) {
    sWindow = window;
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL2_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 420");
    ImGui::StyleColorsDark();
    auto &style = ImGui::GetStyle();
    style.FontSizeBase = 16.0f;
    style.WindowPadding = ImVec2(22, 18);
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(10, 9);
    style.CellPadding = ImVec2(4, 7);
    style.WindowRounding = 10.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 5.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.115f, .135f, .17f, 1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(.09f, .11f, .145f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(.16f, .195f, .25f, 1);
    style.Colors[ImGuiCol_Button] = ImVec4(.19f, .25f, .34f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.26f, .35f, .47f, 1);
    style.Colors[ImGuiCol_Header] = ImVec4(.20f, .285f, .40f, 1);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(.26f, .35f, .47f, 1);
    style.Colors[ImGuiCol_Text] = ImVec4(.92f, .94f, .98f, 1);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(.65f, .72f, .81f, 1);
    sBaseStyle = style;
    AppPadInit();
    AppNetInit();
}

static void UpdateScale() {
    const auto *config = SIM_GetConfigPtr();
    float scale = config->menuScalePercent
        ? config->menuScalePercent / 100.0f
        : ImGui_ImplSDL2_GetContentScaleForWindow(sWindow);
    // SDL reports logical units on Retina/Wayland. Framebuffer density is
    // already handled by the backend; multiplying it here would double-scale.
    if (!std::isfinite(scale) || scale <= 0) scale = 1.0f;
    scale = std::clamp(scale, 1.0f, 3.0f);
    if (std::fabs(scale - sScale) < .001f) return;
    sScale = scale;
    auto &style = ImGui::GetStyle();
    style = sBaseStyle;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

static void DeveloperContents() {
    ImGui::SeparatorText("Performance");
    const double frame = SIM_GetFullFrameTime() / 1000000.0;
    ImGui::Text("Frame: %.2f ms (%.0f FPS)", frame, frame > 0 ? 1000.0 / frame : 0);
    ImGui::Text("CPU render work: %.2f ms", SIM_GetRenderFrameTime() / 1000000.0);
    ImGui::TextWrapped("Renderer: %s", SIM_GetGraphicsRenderer());
    ImGui::Checkbox("Manual pause", &sManualPause);
    ImGui::TextWrapped("Manual pause stays active after closing settings.");
    ProjectPage(Page::Developer);
    ImGui::SeparatorText("Build");
    ImGui::TextWrapped("libntr: %s", SIM_GetLibntrGitHash());
    ImGui::Checkbox("Open ImGui demo", &sShowImGuiDemo);
}

static void DrawPage() {
    switch (sPage) {
    case Page::Display: DisplayContents(); break;
    case Page::Audio: AudioContents(); break;
    case Page::Controls: AppPadContents(); break;
    case Page::GameSpeed:
        ProjectPage(sPage);
        SpeedContents();
        break;
    case Page::Multiplayer: AppNetContents(); break;
    case Page::Developer: DeveloperContents(); break;
    default: ProjectPage(sPage); break;
    }
}

static void FileDialog() {
    auto *dialog = ImGuiFileDialog::Instance();
    if (!dialog->IsOpened("ChooseFileDlgKey")) return;
    auto *viewport = ImGui::GetMainViewport();
    const ImVec2 size(std::max(1.0f, viewport->WorkSize.x * .94f),
                      std::max(1.0f, viewport->WorkSize.y * .94f));
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (dialog->Display("ChooseFileDlgKey", ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings,
                        ImVec2(0, 0), size)) {
        if (dialog->IsOk() && sFileDialogCallback) sFileDialogCallback(dialog->GetFilePathName());
        dialog->Close();
        sFileDialogCallback = nullptr;
    }
}

void Main() {
    if (!sEnabled) return;
    auto *viewport = ImGui::GetMainViewport();
    const ImVec2 size(std::min(980.0f * sScale, viewport->WorkSize.x * .96f),
                      std::min(710.0f * sScale, viewport->WorkSize.y * .94f));
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 115));
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowScroll(ImVec2(0, 0));
    if (sFocusPanel) {
        ImGui::SetNextWindowFocus();
        sFocusPanel = false;
    }
    const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool tight = size.y < ImGui::GetFontSize() * 22.0f;
    if (tight) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * sScale, 10 * sScale));
    ImGui::Begin("Settings & tools###PCSettings", nullptr, flags);
    ImGui::PushFont(nullptr, sBaseStyle.FontSizeBase * 1.25f);
    ImGui::TextUnformatted("Settings & tools");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", IsGameLogicPaused() ? "Game paused" :
        MultiplayerActive() ? "Running (multiplayer)" : "Game running");
    ImGui::Separator();

    const float font = ImGui::GetFontSize();
    const bool compact = ImGui::GetContentRegionAvail().x < font * 40.0f;
    auto *config = SIM_GetConfigPtr();
    char resume[96];
    SDL_snprintf(resume, sizeof(resume), "Resume game (%s)", SDL_GetKeyName(config->padSettings.guiKey));
    const auto &style = ImGui::GetStyle();
    const float footerWidth = ImGui::GetContentRegionAvail().x;
    const float resumeWidth = ImGui::CalcTextSize(resume).x + style.FramePadding.x * 2;
    const float controlsWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
        ImGui::CalcTextSize("Pause while open").x + style.ItemSpacing.x + resumeWidth;
    const bool stackedFooter = footerWidth < controlsWidth;
    const float footer = ImGui::GetFrameHeightWithSpacing() * (stackedFooter ? 2.0f : 1.0f) +
        ImGui::GetStyle().ItemSpacing.y * 2;
    if (compact) {
        int page = static_cast<int>(sPage);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##Category", &page, sPageNames, static_cast<int>(Page::Count))) {
            SelectPage(static_cast<Page>(page));
        }
    }
    const float height = std::max(font * 2, ImGui::GetContentRegionAvail().y - footer);
    if (!compact) {
        ImGui::BeginChild("Categories", ImVec2(font * 11.5f, height), ImGuiChildFlags_Borders);
        for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
            if (i == 0 || i == static_cast<int>(Page::Tools)) {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", i == 0 ? "SETTINGS" : "TOOLS");
            }
            if (ImGui::Selectable(sPageNames[i], static_cast<int>(sPage) == i, 0,
                                  ImVec2(0, ImGui::GetTextLineHeight() + style.FramePadding.y))) {
                SelectPage(static_cast<Page>(i));
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }
    // Separate child IDs keep each page's scroll position across navigation.
    ImGui::PushID(static_cast<int>(sPage));
    ImGui::BeginChild("Page", ImVec2(0, height), ImGuiChildFlags_Borders);
    if (!compact) ImGui::SeparatorText(sPageNames[static_cast<int>(sPage)]);
    ImGui::PushItemWidth(-1);
    DrawPage();
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::PopID();

    ImGui::Separator();
    bool pause = config->pauseWhileMenuOpen;
    if (ImGui::Checkbox("Pause while open", &pause)) {
        config->pauseWhileMenuOpen = pause;
        SIM_Config_SaveConfigFile(config);
    }
    if (!stackedFooter) ImGui::SameLine();
    if (ImGui::Button(resume)) SetEnabled(false);
    const char *saveHint = sPage == Page::Gameplay ? "Gameplay changes use Save" : "Settings save automatically";
    if (footerWidth > (stackedFooter ? resumeWidth : controlsWidth) + style.ItemSpacing.x +
            ImGui::CalcTextSize(saveHint).x) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", saveHint);
    }
    ImGui::End();
    if (tight) ImGui::PopStyleVar();
    if (sEnabled) {
        FileDialog();
        if (sShowImGuiDemo) ImGui::ShowDemoWindow(&sShowImGuiDemo);
    }
}

void OpenFileDialog(std::function<void(std::string)> callback) {
    sFileDialogCallback = callback;
    IGFD::FileDialogConfig config;
    config.path = ".";
    config.flags |= ImGuiFileDialogFlags_Modal;
    ImGuiFileDialog::Instance()->OpenDialog("ChooseFileDlgKey", "Select NDS ROM", ".nds,.srl", config);
}

void NewFrame() {
    const bool hideCursor = !sEnabled &&
        SIM_GetConfigPtr()->screenLayout == SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY &&
        (SDL_GetWindowFlags(sWindow) & SDL_WINDOW_FULLSCREEN) &&
        SDL_GetKeyboardFocus() == sWindow;
    ImGuiIO &io = ImGui::GetIO();
    if (AppPadIsCapturing()) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    if (hideCursor) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    if (SDL_ShowCursor(SDL_QUERY) != (hideCursor ? SDL_DISABLE : SDL_ENABLE))
        SDL_ShowCursor(hideCursor ? SDL_DISABLE : SDL_ENABLE);
    UpdateScale();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

bool ProcessEvent(SDL_Event *event) {
    if (AppPadProcessEvent(event, sEnabled && sPage == Page::Controls)) return true;
#ifdef SDK_BUILD_NX
    if (event->type == SDL_JOYBUTTONDOWN && event->jbutton.button == 8) {
        Toggle();
        return true;
    }
#endif
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        const int scan = event->key.keysym.scancode;
        if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SIM_GetConfigPtr()->padSettings.guiKey) {
            if (!event->key.repeat) Toggle();
            return true;
        }
        if (scan > SDL_SCANCODE_UNKNOWN && scan < SDL_NUM_SCANCODES) {
            const bool suppressed = sSuppressedKeys[scan];
            if (event->type == SDL_KEYUP) sSuppressedKeys[scan] = false;
            else if (sEnabled) sSuppressedKeys[scan] = true;
            if (!sEnabled && suppressed) return true;
        }
    }
    if (!sEnabled) return false;
    if (event->type == SDL_KEYDOWN && !event->key.repeat &&
        event->key.keysym.sym == SDLK_ESCAPE && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        if (!ProjectBack(sPage)) SetEnabled(false);
        return true;
    }
    ImGui_ImplSDL2_ProcessEvent(event);
    // F11 remains usable inside settings, unless a text field owns the key.
    if ((event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) &&
        event->key.keysym.sym == SIM_GetConfigPtr()->padSettings.fullScreenKey &&
        !ImGui::GetIO().WantTextInput)
        return false;
    switch (event->type) {
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_TEXTEDITING:
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: case SDL_MOUSEWHEEL:
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: case SDL_JOYHATMOTION: case SDL_JOYAXISMOTION:
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION:
    case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
        return true;
    default:
        return false;
    }
}

void Render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// Retained for library consumers using the older applet API.
void AppButton(const char *label, bool *state, void (*initFunc)(), void (*appFunc)(bool *)) {
    if (ImGui::Button(label)) {
        if (!*state && initFunc) initFunc();
        *state = !*state;
    }
    if (*state) appFunc(state);
}
}
