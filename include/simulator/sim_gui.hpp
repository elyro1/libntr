#ifndef LIBNTR_SIM_GUI_HPP
#define LIBNTR_SIM_GUI_HPP

#include <nitro/types.h>
#include <SDL2/SDL.h>
#include <functional>
#include <string>

namespace SIM::GUI {

enum class Page { General, Graphics, Controls, Gameplay, Tools, Multiplayer, Developer, Count };

void Init(SDL_Window * window, SDL_GLContext context);
void Main();
void NewFrame();
// Returns true when menu input or a binding capture consumed the event.
bool ProcessEvent(SDL_Event * event);
void Render();
void Toggle();
void Enable();
void OpenFileDialog(std::function<void(std::string)> callback);
bool IsGameLogicPaused();
bool IsEnabled();
// The render loop uses this to release held game input on menu transitions.
bool ConsumeInputReset();
void SelectPage(Page page);

// Project hooks render contents inside the shared panel, without Begin/End.
void ProjectPage(Page page);
bool ProjectBack(Page page);

// Responsive, font-sized settings rows. Call between BeginSettings/EndTable.
bool BeginSettings(const char *id);
void SettingsRow(const char *label, const char *description = nullptr);
void AppButton(const char * label, bool * state, void (*aInitFunc)(void), void (*aAppFunc)(bool *));



}


#endif
