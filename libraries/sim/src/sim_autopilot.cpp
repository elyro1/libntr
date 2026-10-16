// Debug autopilot: plays a script of key presses and takes screenshots, for testing without a person at the keyboard.
// Off unless PLATINUM_AUTOPILOT names a script. Each line is a frame number and a command:
//   <frame> key <SDL key name> <frames held>   e.g. "300 key Z 4"
//   <frame> shot <file.ppm>                     the window as it is drawn that frame
//   <frame> quit
// Lines starting with # are comments. Frames count the rendered frames from start.
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif

namespace {

struct Command {
    unsigned long frame;
    std::string kind;
    std::string arg;
    int hold;
};

struct HeldKey {
    SDL_Keycode key;
    unsigned long releaseFrame;
};

std::vector<Command> sCommands;
std::vector<HeldKey> sHeld;
size_t sNext;
unsigned long sFrame;
bool sLoaded;
bool sEnabled;
std::string sPendingShot;

void Load() {
    sLoaded = true;
    const char *path = std::getenv("PLATINUM_AUTOPILOT");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ss(line);
        Command cmd{};
        cmd.hold = 1;
        ss >> cmd.frame >> cmd.kind >> cmd.arg >> cmd.hold;
        if (!cmd.kind.empty()) {
            sCommands.push_back(cmd);
        }
    }
    sEnabled = true;
    std::fprintf(stderr, "[autopilot] %zu commands from %s\n", sCommands.size(), path);
}

void PushKey(SDL_Keycode key, bool down) {
    SDL_Event ev{};
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.sym = key;
    ev.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    SDL_PushEvent(&ev);
}

} // namespace

// Called once per rendered frame, before the frame's events are read
extern "C" void SIM_Autopilot_Frame(void) {
    if (!sLoaded) {
        Load();
    }
    if (!sEnabled) {
        return;
    }
    sFrame++;
    for (size_t i = 0; i < sHeld.size();) {
        if (sFrame >= sHeld[i].releaseFrame) {
            PushKey(sHeld[i].key, false);
            sHeld.erase(sHeld.begin() + i);
        } else {
            i++;
        }
    }
    while (sNext < sCommands.size() && sCommands[sNext].frame <= sFrame) {
        const Command &cmd = sCommands[sNext++];
        if (cmd.kind == "key") {
            SDL_Keycode key = SDL_GetKeyFromName(cmd.arg.c_str());
            if (key == SDLK_UNKNOWN) {
                std::fprintf(stderr, "[autopilot] unknown key %s\n", cmd.arg.c_str());
                continue;
            }
            PushKey(key, true);
            sHeld.push_back({ key, sFrame + (unsigned long)(cmd.hold > 0 ? cmd.hold : 1) });
        } else if (cmd.kind == "shot") {
            sPendingShot = cmd.arg;
        } else if (cmd.kind == "quit") {
            std::fprintf(stderr, "[autopilot] quit at frame %lu\n", sFrame);
            std::exit(0);
        }
    }
}

// Called just before the frame is presented, with the window's drawable size
extern "C" void SIM_Autopilot_BeforeSwap(int width, int height) {
    if (sPendingShot.empty()) {
        return;
    }
    std::vector<unsigned char> pixels((size_t)width * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    FILE *out = std::fopen(sPendingShot.c_str(), "wb");
    if (out != nullptr) {
        std::fprintf(out, "P6\n%d %d\n255\n", width, height);
        for (int y = height - 1; y >= 0; y--) {
            std::fwrite(&pixels[(size_t)y * width * 3], 1, (size_t)width * 3, out);
        }
        std::fclose(out);
        std::fprintf(stderr, "[autopilot] frame %lu -> %s\n", sFrame, sPendingShot.c_str());
    }
    sPendingShot.clear();
}
