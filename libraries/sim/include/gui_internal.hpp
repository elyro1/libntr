#ifndef LIBNTR_SIM_GUI_INTERNAL_HPP
#define LIBNTR_SIM_GUI_INTERNAL_HPP

namespace SIM::GUI {
void GeneralContents();
void DeveloperLayoutContents();
void GraphicsTimingContents();
void FastForwardContents();
void AppNetInit();
void AppNetContents();
bool AppPadProcessEvent(SDL_Event *event, bool active);
void AppPadInit();
void AppPadContents();
bool AppPadIsCapturing();

}

#endif
