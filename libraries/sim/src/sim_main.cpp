extern "C" void NitroMain();
extern "C" int NitroSpMain(void *arg);
#define SDL_MAIN_HANDLED
#define _POSIX_C_SOURCE 199309L

#include <string>
#include <mutex>
#include <unordered_map>
#include <pthread.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_events.h>

#ifdef SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif
#include <simulator/sim.h>
#include <simulator/sim_audio.h>
#include <simulator/sim_net.h>
#include <simulator/sim_shaders.hpp>

#include <stdio.h>
#include <stdlib.h>

#include <nitro/gx/g3.h>
#include <nitro/gx/gx_load.h>
#include <nitro/gx/gx_vramcnt.h>
#include <nitro/hw/X86/ioreg.h>
#include <nitro/hw/X86/mmap_shared.h>
#include <nitro/os.h>
#include <nitro/pxi/common/fifo.h>

#include <simulator/config/sim_config.h>
#include <simulator/drawmsg.h>
#include <simulator/queue.h>
#include <simulator/simvariables.h>
#include <simulator/g3_draw.h>
#include <simulator/g3_handler.h>
#include <simulator/sim_gui.hpp>
#include <simulator/imgui/imgui.hpp>

#include <math.h>
#include <time.h>

#include "screenquads.h"

#ifdef SDK_BUILD_WIN64
#include <windows.h>
#include <winsock2.h>
#endif

#ifdef SDK_BUILD_LINUX
#include <signal.h>
#endif

#ifdef SDK_BUILD_NX
#include "switch.h"
#endif

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

#include "sim_g2.h"
#include "git_info.h"

// Simulator configuration
SIM_config_type s_SIM_config = {};

// Thread
extern OSThreadInfo OSi_ThreadInfo;

static constexpr u32 s_MaxViewWidth = 448; // 192 * 21 / 9
static u32 s_WindowViewWidth = 256;
static u32 s_FieldViewWidth = 256;
static BOOL s_FieldOverlayTiled = FALSE;
static BOOL s_FieldDarknessMask = FALSE;
static GLuint s_FieldParticlesTexture;
static constexpr GLint s_FieldParticlesTexUnit = 12;
static u8 s_FieldParticlesPixels[4 * SIM_FIELD_SPRITE_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];

static u32 s_TouchPanelCoords[4] = {}; // The bounding box of the touch screen

// SND
#include <nitro/snd/common/work.h>
#include <nitro/snd/common/command.h>

#ifdef SDK_BUILD_WIN64
#define NANOTIME_IMPLEMENTATION
#include "nanotime.h"
#endif

#include <nitro/wm/ARM7/wm_sp.h>

// Graphics Debug Variables
u8 s_SIM_DBG_BGenable[4] = {1, 1, 1, 1};
u8 s_SIM_DBG_OAMenable = 1;
u8 s_SIM_DBG_BGSenable[4] = {1, 1, 1, 1};
u8 s_SIM_DBG_OAMSenable = 1;

u8 s_SIM_useWBuffer = 0;

static struct timespec s_SIM_lastFrameEnd;
static u64 sRenderFrameTime;
static u64 sFullFrameTime;
static bool sFastForwardActive = false;
#if defined(SDK_BUILD_LINUX) || defined(SDK_BUILD_WIN64)
static unsigned char sExtendedTextures[4 * 1024 * 1024];
struct VirtualTexture { u32 address; u32 flags; };
static VirtualTexture sVirtualTextures[65536];
static std::unordered_map<u64, u16> sVirtualTextureIDs;
static std::mutex sVirtualTextureMutex;

extern "C" void SIM_ResetVirtualTextureAddresses(void) {
  // Finish queued draws before IDs can be reused by a new graphics pipeline.
  G3SIM_FlushArray();
  G3SIM_DrawItems();
  std::lock_guard<std::mutex> lock(sVirtualTextureMutex);
  sVirtualTextureIDs.clear();
  memset(sVirtualTextures, 0, sizeof(sVirtualTextures));
}

extern "C" void *SIM_GetExtendedTextureStorage(u32 address, u32 size) {
  if (address < 0x80000 || address > sizeof(sExtendedTextures) ||
      size > sizeof(sExtendedTextures) - address) return nullptr;
  return sExtendedTextures + address;
}

extern "C" u32 SIM_EncodeTextureAddress(u32 parameter, u32 baseAddress) {
  const u32 address = baseAddress + ((parameter & 0xffff) << 3);
  if (address < 0x80000) return (parameter & ~0xffffU) | (address >> 3);
  const u32 flags = parameter & 0x3c000000;
  const u64 identity = ((u64)flags << 32) | address;
  std::lock_guard<std::mutex> lock(sVirtualTextureMutex);
  auto found = sVirtualTextureIDs.find(identity);
  u16 id;
  if (found != sVirtualTextureIDs.end()) id = found->second;
  else {
    if (sVirtualTextureIDs.size() >= 65535) { fprintf(stderr, "Virtual texture IDs exhausted\n"); abort(); }
    id = (u16)(sVirtualTextureIDs.size() + 1);
    sVirtualTextureIDs.emplace(identity, id);
    sVirtualTextures[id] = {address, flags};
  }
  return (parameter & ~0x3c00ffffU) | 0x20000000 | id;
}

extern "C" u32 SIM_DecodeTextureAddress(u32 *parameter) {
  if ((*parameter & 0x3c000000) == 0x20000000 && (*parameter & 0xffff)) {
    std::lock_guard<std::mutex> lock(sVirtualTextureMutex);
    const auto entry = sVirtualTextures[*parameter & 0xffff];
    if (entry.address) {
      *parameter = (*parameter & ~0x3c00ffffU) | entry.flags;
      return entry.address;
    }
  }
  return (*parameter & 0xffff) << 3;
}
#endif

static constexpr u64 sNormalFrameBudgetNs = 16600000;
static float sEffectiveSpeed = 1.0f;

static void UpdateFastForward(SDL_Window *gameWindow) {
  const Uint8 *keys = SDL_GetKeyboardState(nullptr);
  const SDL_Scancode fastForwardKey = SDL_GetScancodeFromKey(s_SIM_config.padSettings.fastForwardKey);
  sFastForwardActive = SDL_GetKeyboardFocus() == gameWindow &&
                       !ImGui::GetIO().WantCaptureKeyboard &&
                       !SIM::GUI::IsGameLogicPaused() &&
                       fastForwardKey != SDL_SCANCODE_UNKNOWN &&
                       keys[fastForwardKey];
  sEffectiveSpeed = sFastForwardActive
      ? SIM_Config_ValidateFastForwardMultiplier(s_SIM_config.fastForwardMultiplier)
      : 1.0f;
  SIM_Audio_SetFastForwardMultiplier(sEffectiveSpeed);

  // Override VSync only while speeding up; keep the saved setting intact.
  const int swapInterval = sFastForwardActive ? 0 : s_SIM_config.vsyncInterval;
  if (SDL_GL_GetSwapInterval() != swapInterval) {
    SDL_GL_SetSwapInterval(swapInterval);
  }
}

static void WaitForFrameBudget(u64 frameNs, u64 budgetNs) {
  while (frameNs < budgetNs) {
    const u64 remainingMs = (budgetNs - frameNs) / 1000000;
    // Sleep most of the wait, then yield until the deadline. A millisecond-only
    // delay is too coarse to pace the 3.32 ms fast-forward frames accurately.
    SDL_Delay(remainingMs > 1 ? remainingMs - 1 : 0);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    frameNs = ((now.tv_sec - s_SIM_lastFrameEnd.tv_sec) * 1000000000) +
              (now.tv_nsec - s_SIM_lastFrameEnd.tv_nsec);
  }
}

u8 bgtex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg0tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg1tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg2tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg3tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg0stex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg1stex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg2stex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 bg3stex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];

u8 oam0Tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam1Tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam2Tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam3Tex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam0sTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam1sTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam2sTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 oam3sTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 objWindowTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];
u8 objWindowsTex[4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2];

u32 OBJLine[2][256];
u8 OBJWindow[2][256];

GXVRamTex s_SIM_GXVRamTex;
GXVRamTexPltt s_SIM_GXVRamTexPltt;
GXVRamBGExtPltt s_SIM_GXBgExtPltt;
GXVRamSubBGExtPltt s_SIM_GXSubBgExtPltt;

// 4MB! can we reduce this?
u8 s_SIM_g3tex[4 * 1024 * 1024];

// FBOs
static GLuint g3FrameBuffer;
static GLuint fboId;
static GLuint g2FrameBuffer;
static GLuint s_SideAverageFramebuffer;
static GLuint s_SideAverageTexture;
static bool s_SideAverageReady;
static constexpr GLint s_SideAverageTexUnit = 10;

// Texture Objects
GLuint s_bgTextureId[4];
GLuint s_objTextureId[5];
static GLuint g2TextureId;
static GLuint g3RenderTextureId;
static GLuint g3FogTextureId;
static GLuint dummyScreenTextureId;
static GLuint objWindowTextureId;

GLint s_bgTexUnits[] = {0, 1, 2, 3};
static GLint s_objTexUnits[] = {4, 5, 6, 7, 8};
static GLint s_objWindowTexUnit = 9;

// Shader Programs
GLuint g3shaderProgramID;
static GLuint g2shaderProgramID;

// VAOs
static GLuint g2vertexArray;

// VBOs
static GLuint g2vertexBuffer;

// RBOs
static GLuint g3RenderBufferId;

// Shaders
static GLint g2VertexShader;
static GLint g2FragmentShader;
static GLint myVertexShader;
static GLint myFragmentShader;

// GL Context
static SDL_GLContext context;

SDL_Window *window;

u8 s_SIM_dbg_dumpSound = 0;
static u8 s_SIM_dbg_soundFileOpen = 0;
static FILE *s_SIM_dbg_soundFile = NULL;
u8 s_SIM_dbg_soundSoloChannel = 0xFF;
void *win_snd_channel_bufs[32] = {0};
WIN_snd_channel_info win_snd_channels[32];
WIN_snd_channel_info win_sndi_channels[16] = {0};
WIN_snd_alarm_info win_snd_alarms[32] = {0};
SNDSharedWork *win_SNDi_SharedWork;

OSIrqFunction OS_IRQTable[OS_IRQ_TABLE_MAX] = {
    OS_IrqDummy, // VBlank (for ARM9)
    OS_IrqDummy, // HBlank
    OS_IrqDummy, // VCounter
    OS_IrqDummy, // timer0
    OS_IrqDummy, // timer1
    OS_IrqDummy, // timer2
    OS_IrqDummy, // timer3
    OS_IrqDummy, // serial communication (will not occur)
    OS_IrqDummy, // DMA0
    OS_IrqDummy, // DMA1
    OS_IrqDummy, // DMA2
    OS_IrqDummy, // DMA3
    OS_IrqDummy, // key
    OS_IrqDummy, // cartridge
    OS_IrqDummy, // (not used)
    OS_IrqDummy, // (not used)
    OS_IrqDummy, // sub processor
    OS_IrqDummy, // sub processor send FIFO empty
    OS_IrqDummy, // sub processor receive FIFO not empty
    OS_IrqDummy, // card data transfer finish
    OS_IrqDummy, // card IREQ
    OS_IrqDummy, // geometry command FIFO
    OS_IrqDummy, // Power Management IC
    OS_IrqDummy, // SPI data transfer
    OS_IrqDummy  // Wireless module
};

u8 s_mosaicTable[16][256];

static u8 isMouseDown = 0;

// Local Procedure Declarations
static void ConvertMsgParams(draw_msg_t *msg);
static u8 GetNumParamsFromCommandType(draw_command_type_t type);
static u8 GetParamSizeFromCommandType(draw_command_type_t type);
static u8 isParamSigned(draw_command_type_t type);
static u64 map(u64 x, u64 in_min, u64 in_max, u64 out_min, u64 out_max);
static void process_nibble(unsigned char data4bit, int *Index, int *Pcm16bit);
static void DrawScreenQuad();
static void DrawEngine(BOOL isSub);

static u8 s_numG3CommandsThisFrame = 0;

// Vcount stuff
static SDL_sem *vcountResetSemaphore;
static SDL_sem *vcountVblankSemaphore;

SDL_Joystick *s_SIM_SDLJoystick = NULL;

static void DrawScreenQuad() {
#ifdef SDK_TRACY_ENABLE
  TracyCZone(ctx, 1);
#endif
  glBindVertexArray(g2vertexArray);
  glBindBuffer(GL_ARRAY_BUFFER, g2vertexBuffer);
  G3SIM_Vertex_t *quadArray = sim_GetScreenQuadArray(s_SIM_config.screenLayout,
                                                     s_SIM_config.swapScreens);
  int arrayCount = sim_GetScreenQuadVertexCount(s_SIM_config.screenLayout,
                                                s_SIM_config.swapScreens);

  glBufferData(GL_ARRAY_BUFFER, sizeof(G3SIM_Vertex_t) * arrayCount, quadArray,
               GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t),
                        (void *)offsetof(G3SIM_Vertex_t, x));
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t),
                        (void *)offsetof(G3SIM_Vertex_t, s));

  glDrawArrays(GL_TRIANGLES, 0, arrayCount);
#ifdef SDK_TRACY_ENABLE
  TracyCZoneEnd(ctx);
#endif
}

SIM_config_type *SIM_GetConfigPtr() { return &s_SIM_config; }

static u64 map(u64 x, u64 in_min, u64 in_max, u64 out_min, u64 out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void SIM_u16ToRGB(u16 in, u8 *r, u8 *g, u8 *b) {
  *r = ( (in & 0x1F) * 527 + 23 ) >> 6;
  *g = ( ((in >> 5) & 0x1F) * 527 + 23 ) >> 6;
  *b = ( ((in >> 10) & 0x1F) * 527 + 23 ) >> 6;
}

void *runVblankInterrupt(void *arg) {
  OS_IRQTable[OS_IE_V_BLANK - 1]();
  return NULL;
}

void GLAPIENTRY MessageCallback(GLenum source, GLenum type, GLuint id,
                                GLenum severity, GLsizei length,
                                const GLchar *message, const void *userParam) {
  // fprintf( stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message =
  // %s\n",
  //          ( type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : "" ),
  //           type, severity, message );
}

static u32 sPendingInternalResolution;

void SIM_SetInternalResolutionAfterRender(int scale) {
  sPendingInternalResolution = scale < 1 ? 1 : (scale > 20 ? 20 : scale);
}

static void ApplyPendingInternalResolution() {
  if (!sPendingInternalResolution) return;
  const u32 scale = sPendingInternalResolution;
  sPendingInternalResolution = 0;
  if (scale == s_SIM_config.internalResolutionScale) return;
  GLint maxTextureSize, maxRenderbufferSize;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbufferSize);
  const u32 maxDimension = s_MaxViewWidth > SIM_NDS_SCREEN_HEIGHT * 2
      ? s_MaxViewWidth : SIM_NDS_SCREEN_HEIGHT * 2;
  if (maxDimension * scale > (u32)maxTextureSize ||
      maxDimension * scale > (u32)maxRenderbufferSize) return;
  GLint texture, renderbuffer, activeTexture;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
  for (GLuint id : {g3RenderTextureId, g3FogTextureId}) {
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_MaxViewWidth * scale,
                 SIM_NDS_SCREEN_HEIGHT * 2 * scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  glBindRenderbuffer(GL_RENDERBUFFER, g3RenderBufferId);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
      s_MaxViewWidth * scale, SIM_NDS_SCREEN_HEIGHT * 2 * scale);
  // New storage has undefined contents; initialize both color attachments and
  // the depth/stencil buffer before the game starts the next 3D frame.
  GLint framebuffer, stencilMask;
  GLboolean depthMask;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
  glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g3FrameBuffer);
  const GLfloat clearColor[] = {0, 0, 0, 0};
  glClearBufferfv(GL_COLOR, 0, clearColor);
  glClearBufferfv(GL_COLOR, 1, clearColor);
  glDepthMask(GL_TRUE);
  glStencilMask(0xff);
  glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
  glDepthMask(depthMask);
  glStencilMask(stencilMask);
  if (scissor) glEnable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
  glActiveTexture(activeTexture);
  s_SIM_config.internalResolutionScale = scale;
  SIM_Config_SaveConfigFile(&s_SIM_config);
}

void *SIM_RenderInit(void *arg) {

  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER |
           SDL_INIT_JOYSTICK);
  SIM_Audio_Init(44100);

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetSwapInterval(s_SIM_config.vsyncInterval);

  u32 windowHeight;
  u32 windowWidth;

  if (s_SIM_config.windowWidth == 0 || s_SIM_config.windowHeight == 0) {
    switch (s_SIM_config.screenLayout) {
    default:
    case SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY:
      windowHeight = 576;
      windowWidth = 1024;
      break;
    case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
      windowHeight = SIM_NDS_SCREEN_HEIGHT * 2 * 2;
      windowWidth = SIM_NDS_SCREEN_WIDTH * 2;
      break;
    case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
      windowHeight = SIM_NDS_SCREEN_HEIGHT * 2;
      windowWidth = SIM_NDS_SCREEN_WIDTH * 2 * 2;
      break;
    case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
      windowHeight = SIM_NDS_SCREEN_HEIGHT * 2 * 2;
      windowWidth = (SIM_NDS_SCREEN_WIDTH * 2 * 2) + (SIM_NDS_SCREEN_WIDTH * 2);
      break;
    }
  } else {
    windowHeight = s_SIM_config.windowHeight;
    windowWidth = s_SIM_config.windowWidth;
  }

  std::string windowTitle = "libntr Application";

  CARDRomHeader *cardHeader = (CARDRomHeader *)(HW_ROM_HEADER_BUF);

  if (strlen(cardHeader->game_name) > 0) {
    char gameName[13] = {0};
    char gameCode[5] = {0};
    char makerCode[3] = {0};

    strncpy(gameName, cardHeader->game_name, 12);
    strncpy(gameCode, (char *)&cardHeader->game_code, 4);
    strncpy(makerCode, (char *)&cardHeader->maker_code, 2);

    windowTitle = std::string(gameName) + " [" + std::string(gameCode) +
                  std::string(makerCode) + "]";
  }

  window = SDL_CreateWindow(
      windowTitle.c_str(), 100, 100, windowWidth, windowHeight,
      SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  context = SDL_GL_CreateContext(window);

  gladLoadGLLoader(SDL_GL_GetProcAddress);

  glViewport(0, 0, windowWidth, windowHeight);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

  glEnable(GL_DEBUG_OUTPUT);
  glDebugMessageCallback(MessageCallback, 0);

  GLenum glErrCode;
  GLint status;
  GLint logSize = 0;

  GLchar *errorBuf;

  glGenTextures(4, s_bgTextureId);
  glActiveTexture(GL_TEXTURE0);

  for (int i = 0; i < 4; i++) {
    glActiveTexture(GL_TEXTURE0 + s_bgTexUnits[i]);
    glBindTexture(GL_TEXTURE_2D, s_bgTextureId[i]);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SIM_NDS_SCREEN_WIDTH,
                 SIM_NDS_SCREEN_HEIGHT * 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  }

  glGenTextures(1, &s_FieldParticlesTexture);
  glActiveTexture(GL_TEXTURE0 + s_FieldParticlesTexUnit);
  glBindTexture(GL_TEXTURE_2D_ARRAY, s_FieldParticlesTexture);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, SIM_FIELD_SPRITE_WIDTH,
              SIM_NDS_SCREEN_HEIGHT * 2, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  glGenTextures(5, s_objTextureId);
  glActiveTexture(GL_TEXTURE0);

  for (int i = 0; i < 5; i++) {
    glActiveTexture(GL_TEXTURE0 + s_objTexUnits[i]);
    glBindTexture(GL_TEXTURE_2D, s_objTextureId[i]);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SIM_NDS_SCREEN_WIDTH,
                 SIM_NDS_SCREEN_HEIGHT * 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  }

#define GenerateTexture(_id, _width, _height)                                  \
  glGenTextures(1, &_id);                                                      \
  glActiveTexture(GL_TEXTURE0);                                                \
  glBindTexture(GL_TEXTURE_2D, _id);                                           \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);           \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);           \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);                \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);                \
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _width, _height, 0, GL_RGBA,        \
               GL_UNSIGNED_BYTE, NULL);

#define CompileShaderCommon(_id, _source)                                      \
  glShaderSource(_id, 1, &_source, NULL);                                      \
  glCompileShader(_id);                                                        \
  glGetShaderiv(_id, GL_COMPILE_STATUS, &status);                              \
  if (status != GL_TRUE) {                                                     \
    glGetShaderiv(g2FragmentShader, GL_INFO_LOG_LENGTH, &logSize);             \
    errorBuf = static_cast<GLchar *>(malloc(logSize * sizeof(GLchar)));        \
    glGetShaderInfoLog(_id, logSize, &logSize, (GLchar *)errorBuf);            \
    printf("ERROR COMPILING _source SHADER:\n%s\n", errorBuf);                 \
    free(errorBuf);                                                            \
  }

#define CompileVertexShader(_id, _source)                                      \
  _id = glCreateShader(GL_VERTEX_SHADER);                                      \
  CompileShaderCommon(_id, _source);

#define CompileFragmentShader(_id, _source)                                    \
  _id = glCreateShader(GL_FRAGMENT_SHADER);                                    \
  CompileShaderCommon(_id, _source);

#define LinkShader(_id, _vertex, _fragment)                                    \
  glAttachShader(_id, _vertex);                                                \
  glAttachShader(_id, _fragment);                                              \
  glLinkProgram(_id);                                                          \
  glErrCode = glGetError();                                                    \
  glGetProgramiv(_id, GL_LINK_STATUS, &status);                                \
  if (status != GL_TRUE) {                                                     \
    glGetProgramiv(_id, GL_INFO_LOG_LENGTH, &logSize);                         \
    errorBuf = (GLchar *)malloc(logSize * sizeof(GLchar));                     \
    glGetProgramInfoLog(_id, logSize, &logSize, (GLchar *)errorBuf);           \
    glErrCode = glGetError();                                                  \
    printf("ERROR LINKING _id SHADER:\n%s\n", errorBuf);                       \
    free(errorBuf);                                                            \
  }                                                                            \
  glValidateProgram(_id);                                                      \
  glUseProgram(_id);

  GenerateTexture(dummyScreenTextureId, SIM_NDS_SCREEN_WIDTH,
                  SIM_NDS_SCREEN_HEIGHT * 2);
  GenerateTexture(objWindowTextureId, SIM_NDS_SCREEN_WIDTH,
                  SIM_NDS_SCREEN_HEIGHT * 2);

  // 3D Framebuffer render texture
  glGenFramebuffers(1, &g3FrameBuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, g3FrameBuffer);
  glGenTextures(1, &g3RenderTextureId);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g3RenderTextureId);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
               s_MaxViewWidth * s_SIM_config.internalResolutionScale,
               SIM_NDS_SCREEN_HEIGHT * 2 * s_SIM_config.internalResolutionScale,
               0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g3RenderTextureId, 0);
  // Keep each pixel's fog state separate from the blended scene color.
  glGenTextures(1, &g3FogTextureId);
  glBindTexture(GL_TEXTURE_2D, g3FogTextureId);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
               s_MaxViewWidth * s_SIM_config.internalResolutionScale,
               SIM_NDS_SCREEN_HEIGHT * 2 * s_SIM_config.internalResolutionScale,
               0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                         g3FogTextureId, 0);
  const GLenum g3DrawBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glDrawBuffers(2, g3DrawBuffers);
  glGenRenderbuffers(1, &g3RenderBufferId);
  glBindRenderbuffer(GL_RENDERBUFFER, g3RenderBufferId);
  glRenderbufferStorage(
      GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
      s_MaxViewWidth * s_SIM_config.internalResolutionScale,
      SIM_NDS_SCREEN_HEIGHT * 2 * s_SIM_config.internalResolutionScale);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, g3RenderBufferId);

  glStencilMask(0xff);
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);

  // Check FBO status
  int fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
    printf("Framebuffer error: %d\n", fboStatus);
  }

  // Unbind all that stuff
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glBindTexture(GL_TEXTURE_2D, g2TextureId);

  glGenFramebuffers(1, &g2FrameBuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Reduce a composed screen to a single color entirely on the GPU. A square,
  // power-of-two target gives every part of the screen equal weight in mipmaps.
  glGenTextures(1, &s_SideAverageTexture);
  glActiveTexture(GL_TEXTURE0 + s_SideAverageTexUnit);
  glBindTexture(GL_TEXTURE_2D, s_SideAverageTexture);
  glTexStorage2D(GL_TEXTURE_2D, 9, GL_RGBA16F, 256, 256);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &s_SideAverageFramebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, s_SideAverageFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         s_SideAverageTexture, 0);
  s_SideAverageReady = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (!s_SideAverageReady) {
    printf("Side gradient framebuffer unavailable; using backdrop color.\n");
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
  glActiveTexture(GL_TEXTURE0);

  // Setup G3 Shader Program
  g3shaderProgramID = glCreateProgram();
  CompileVertexShader(myVertexShader, SIM::G3VertexShader);
  CompileFragmentShader(myFragmentShader, SIM::G3FragmentShader);
  LinkShader(g3shaderProgramID, myVertexShader, myFragmentShader);

  // Setup G2 Shader Program
  g2shaderProgramID = glCreateProgram();
  CompileVertexShader(g2VertexShader, SIM::G2VertexShader);
  CompileFragmentShader(g2FragmentShader, SIM::G2FragmentShader);
  LinkShader(g2shaderProgramID, g2VertexShader, g2FragmentShader);

  GLint texUnitLoc = glGetUniformLocation(g2shaderProgramID, "myTexture");
  glProgramUniform1i(g2shaderProgramID, texUnitLoc, 0);

  // Set up the texture units in the shader
  glUniform1iv(glGetUniformLocation(g2shaderProgramID, "bgTexture"), 4,
               s_bgTexUnits);
  glUniform1iv(glGetUniformLocation(g2shaderProgramID, "objTexture"), 5,
               s_objTexUnits);

  glGenVertexArrays(1, &g2vertexArray);
  glBindVertexArray(g2vertexArray);
  glGenBuffers(1, &g2vertexBuffer);

  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                      GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  SDL_GL_SwapWindow(window);

  // Setup ImGui
  SIM::GUI::Init(window, context);

  clock_gettime(CLOCK_MONOTONIC, &s_SIM_lastFrameEnd);

  return nullptr;
}

static void DrawEngine(BOOL isSub) {
  const char *zoneName;
  zoneName = isSub ? "DrawEngine (SUB)" : "DrawEngine (MAIN)";
#ifdef SDK_TRACY_ENABLE
  TracyCZone(ctx, 1);
  TracyCZoneName(ctx, zoneName, strlen(zoneName));
#endif
  u8 bg0as3d;
  u8 maxPriority;
  u8 maxPriorityBG;
  u8 bgEnabled[4];
  u8 bgMode;
  GLint bgOrder[4];
  GLint bgPriorities[4];

  u32 dispSel = s_reg_GX_POWCNT >> 15;
  const bool singleScreen = s_SIM_config.screenLayout == SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY;
  if (singleScreen && !(dispSel ? !isSub : isSub)) {
    return;
  }

  // Get the enabled BGs
  u32 dispcnt = isSub ? s_reg_GXS_DB_DISPCNT : s_reg_GX_DISPCNT;

  bgEnabled[0] = (dispcnt >> 8) & 0x01;
  bg0as3d = !isSub && ((dispcnt >> 3) & 0x01);
  bgEnabled[1] = (dispcnt >> 8) & 0x02;
  bgEnabled[2] = (dispcnt >> 8) & 0x04;
  bgEnabled[3] = (dispcnt >> 8) & 0x08;

  // Get the graphics mode
  bgMode = (u8)(dispcnt & 0x7);

  // Get the backdrop color
  u8 backdropR;
  u8 backdropG;
  u8 backdropB;
  u16 backdropColor;
  backdropColor =
      isSub ? *(u16 *)&s_HW_DB_BG_PLTT[0] : *(u16 *)&s_HW_BG_PLTT[0];
  SIM_u16ToRGB(backdropColor, &backdropR, &backdropG, &backdropB);
  float backdropVec[4];

  backdropVec[0] = (float)backdropR / 255.0f;
  backdropVec[1] = (float)backdropG / 255.0f;
  backdropVec[2] = (float)backdropB / 255.0f;
  backdropVec[3] = 1.0f;

  glUniform1fv(glGetUniformLocation(g2shaderProgramID, "backdropColor"), 4,
               backdropVec);

  glProgramUniform4f(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "backdropColor"),
                     (float)backdropR / 255.0f, (float)backdropG / 255.0f,
                     (float)backdropB / 255.0f, 1.0f);

  // Get the BG priorities
  u16 bg0cnt = isSub ? s_reg_G2S_DB_BG0CNT : s_reg_G2_BG0CNT;
  u16 bg1cnt = isSub ? s_reg_G2S_DB_BG1CNT : s_reg_G2_BG1CNT;
  u16 bg2cnt = isSub ? s_reg_G2S_DB_BG2CNT : s_reg_G2_BG2CNT;
  u16 bg3cnt = isSub ? s_reg_G2S_DB_BG3CNT : s_reg_G2_BG3CNT;

  bgPriorities[0] = bg0cnt & (u16)3;
  bgPriorities[1] = bg1cnt & (u16)3;
  bgPriorities[2] = bg2cnt & (u16)3;
  bgPriorities[3] = bg3cnt & (u16)3;

  // Set up the BGPriorities in the shader
  glUniform1iv(glGetUniformLocation(g2shaderProgramID, "bgPriorities"), 4,
               bgPriorities);

  // Determine the order to draw the BGs
  bgOrder[0] = 0;
  bgOrder[1] = 1;
  bgOrder[2] = 2;
  bgOrder[3] = 3;

  for (int i = 0; i < 4; i++) {
    maxPriorityBG = 0;
    maxPriority = 0;
    for (int j = 0; j < 4; j++) {
      if (bgPriorities[j] == 0xFF) {
        continue;
      }
      if (bgPriorities[j] >= maxPriority) {
        maxPriorityBG = j;
        maxPriority = bgPriorities[j];
      }
    }
    bgOrder[i] = maxPriorityBG;
    bgPriorities[maxPriorityBG] = 0xFF;
  }

  // Set up the BGOrder in the shader
  glUniform1iv(glGetUniformLocation(g2shaderProgramID, "bgOrder"), 4, bgOrder);

  // Clear out the textures
  for (int i = 0; i < 4; i++) {
    glActiveTexture(GL_TEXTURE0 + s_bgTexUnits[0]);
    glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
  }

  for (int i = 0; i < 5; i++) {
    glActiveTexture(GL_TEXTURE0 + s_objTexUnits[0]);
    glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
  }

  // Draw the Sprites
  if (isSub) {
    memset(oam0sTex, 0, sizeof(oam0sTex));
    memset(oam1sTex, 0, sizeof(oam1sTex));
    memset(oam2sTex, 0, sizeof(oam2sTex));
    memset(oam3sTex, 0, sizeof(oam3sTex));
    memset(objWindowsTex, 0, sizeof(objWindowsTex));
  } else {
    memset(oam0Tex, 0, sizeof(oam0Tex));
    memset(oam1Tex, 0, sizeof(oam1Tex));
    memset(oam2Tex, 0, sizeof(oam2Tex));
    memset(oam3Tex, 0, sizeof(oam3Tex));
    memset(objWindowTex, 0, sizeof(objWindowTex));
  }

  for (int i = 0; i < SIM_NDS_SCREEN_HEIGHT; i++) {
    for (int bgnum = 0x0C00; bgnum >= 0x0000; bgnum -= 0x0400) {
      memset(OBJLine, 0, sizeof(OBJLine));
      memset(OBJWindow, 0, sizeof(OBJWindow));
      G2SIM_DrawOBJ(i, isSub, bgnum, FALSE);
      u8 *pixelBuf;
      u8 *objWindowBuf = isSub ? objWindowsTex : objWindowTex;
      switch (bgnum) {
      case 0xC00:
        pixelBuf = isSub ? oam3sTex : oam3Tex;
        break;
      case 0x800:
        pixelBuf = isSub ? oam2sTex : oam2Tex;
        break;
      case 0x400:
        pixelBuf = isSub ? oam1sTex : oam1Tex;
        break;
      case 0x000:
        pixelBuf = isSub ? oam0sTex : oam0Tex;
        break;
      default:
        // Invalid case
        pixelBuf = NULL;
        break;
      }
      u8 r;
      u8 g;
      u8 b;
      u8 a;
      for (int j = 0; j < 256; j++) {
        if (OBJLine[isSub][j]) {
          u8 colorIdx = OBJLine[isSub][j] & 0xFF;
          if (colorIdx) {
            u16 *objPltt =
                isSub ? (u16 *)s_HW_DB_OBJ_PLTT : (u16 *)s_HW_OBJ_PLTT;
            u16 color = objPltt[colorIdx];
            SIM_u16ToRGB(color, &r, &g, &b);
            G2SIM_PlotPixel(pixelBuf, r, g, b, j, i, isSub);
          }
        }
        if (OBJWindow[isSub][j]) {
          G2SIM_PlotPixel(objWindowBuf, 255, 255, 255, j, i, isSub);
        }
      }
    }
  }

  // Keep the native OBJ composition intact. The extra texture contains only
  // registered field sprites rendered beyond native width, without clipping their edges.
  const bool fieldParticlesEnabled = !isSub && s_FieldViewWidth > SIM_NDS_SCREEN_WIDTH
                                     && G2SIM_HasFieldSprites();
  if (fieldParticlesEnabled) {
    glActiveTexture(GL_TEXTURE0 + s_FieldParticlesTexUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_FieldParticlesTexture);
    for (int priority = 0; priority < 4; priority++) {
      memset(s_FieldParticlesPixels, 0, sizeof(s_FieldParticlesPixels));
      for (int line = 0; line < SIM_NDS_SCREEN_HEIGHT; line++) {
        memset(s_FieldSpriteLine, 0, sizeof(u32) * SIM_FIELD_SPRITE_WIDTH);
        G2SIM_DrawOBJ(line, FALSE, priority << 10, TRUE);
        for (int x = 0; x < SIM_FIELD_SPRITE_WIDTH; x++) {
          const u8 index = s_FieldSpriteLine[x] & 0xFF;
          if (index) {
            u8 r, g, b;
            SIM_u16ToRGB(((u16 *)s_HW_OBJ_PLTT)[index], &r, &g, &b);
            const int row = (s_reg_GX_POWCNT >> 15) ? 383 - line : 191 - line;
            u8 *pixel = s_FieldParticlesPixels + 4 * (row * SIM_FIELD_SPRITE_WIDTH + x);
            pixel[0] = r; pixel[1] = g; pixel[2] = b; pixel[3] = 255;
          }
        }
      }
      glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, priority,
                     SIM_FIELD_SPRITE_WIDTH, SIM_NDS_SCREEN_HEIGHT * 2, 1,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_FieldParticlesPixels);
    }
  }
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "fieldParticlesTexture"),
                     s_FieldParticlesTexUnit);

  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "fieldParticlesEnabled"),
                     fieldParticlesEnabled);

  int drawnOAMs = 0;

  // Draw the BGs
  for (int i = 0; i < 4; i++) {
    u8 curBgNum = bgOrder[i];

    if (bgEnabled[bgOrder[i]]) {
      G2SIM_DrawBG(bgOrder[i], bgMode, bg0as3d, isSub);
    } else {
      glActiveTexture(GL_TEXTURE0 + s_bgTexUnits[bgOrder[i]]);
      glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
    }
  }

  // write all OAMs to the textures based on their priorities
  for (int i = 0; i < 4; i++) {
    glActiveTexture(GL_TEXTURE0 + s_objTexUnits[i]);
    glBindTexture(GL_TEXTURE_2D, s_objTextureId[i]);
    void *oamPixelBuf;
    switch (i) {
    case 0:
      oamPixelBuf = isSub ? oam0sTex : oam0Tex;
      break;
    case 1:
      oamPixelBuf = isSub ? oam1sTex : oam1Tex;
      break;
    case 2:
      oamPixelBuf = isSub ? oam2sTex : oam2Tex;
      break;
    case 3:
      oamPixelBuf = isSub ? oam3sTex : oam3Tex;
      break;
    default:
      // invalid
      oamPixelBuf = NULL;
      break;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SIM_NDS_SCREEN_WIDTH,
                    SIM_NDS_SCREEN_HEIGHT * 2, GL_RGBA, GL_UNSIGNED_BYTE,
                    (void *)oamPixelBuf);
  }

  glActiveTexture(GL_TEXTURE0 + s_bgTexUnits[0]);

  if (bgEnabled[0]) {
    if (bg0as3d) {
      glBindTexture(GL_TEXTURE_2D, g3RenderTextureId);
    } else {
      glBindTexture(GL_TEXTURE_2D, s_bgTextureId[0]);
    }
  } else {
    glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
  }

  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, g3FogTextureId);
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "g3FogTexture"), 11);
  glProgramUniform1f(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "g3FogAlpha"),
                     (float)((s_reg_G3X_FOG_COLOR >> 16) & 31) / 32.0f);
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "g3FogAlphaOnly"),
                     (s_reg_G3X_DISP3DCNT & (1 << 6)) != 0);

  // Setup the OBJWindow texture
  glActiveTexture(GL_TEXTURE0 + s_objWindowTexUnit);
  glBindTexture(GL_TEXTURE_2D, objWindowTextureId);
  u8 *objWindowBuf = isSub ? objWindowsTex : objWindowTex;
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SIM_NDS_SCREEN_WIDTH,
                  SIM_NDS_SCREEN_HEIGHT * 2, GL_RGBA, GL_UNSIGNED_BYTE,
                  (void *)objWindowBuf);
  glProgramUniform1i(
      g2shaderProgramID,
      glGetUniformLocation(g2shaderProgramID, "objWindowTexture"),
      s_objWindowTexUnit);

  // Pass BLDCNT, BLDALPHA, and BLDY value to the shader
  u16 bldcnt = isSub ? s_reg_G2S_DB_BLDCNT : s_reg_G2_BLDCNT;
  u16 bldalpha = isSub ? s_reg_G2S_DB_BLDALPHA : s_reg_G2_BLDALPHA;
  u16 bldy = isSub ? s_reg_G2S_DB_BLDY : s_reg_G2_BLDY;
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "bldcnt"), bldcnt);
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "bldalpha"),
                     bldalpha);
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "bldy"), bldy);

  int topScreen = dispSel ? !isSub : isSub;
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "topScreen"),
                     topScreen);

  // Pass DISPCNT and WINOUT to the shader
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "dispcnt"),
                     dispcnt);
  u16 winout = isSub ? s_reg_G2S_DB_WINOUT : s_reg_G2_WINOUT;
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "winout"), winout);

  // Pass bg0as3d to the shader
  glProgramUniform1i(g2shaderProgramID,
                     glGetUniformLocation(g2shaderProgramID, "bg0as3d"),
                     bg0as3d);

  // Pass master bright info to the shader
  float masterBrightFactor = isSub
                                 ? (float)(s_reg_GXS_DB_MASTER_BRIGHT & 0b11111)
                                 : (float)(s_reg_GX_MASTER_BRIGHT & 0b11111);
  if (masterBrightFactor > 16.0f) {
    masterBrightFactor = 16.0f;
  }
  glProgramUniform1f(
      g2shaderProgramID,
      glGetUniformLocation(g2shaderProgramID, "inMasterBrightFactor"),
      masterBrightFactor);
  int masterBrightMode = isSub ? ((s_reg_GXS_DB_MASTER_BRIGHT >> 14) & 3)
                               : ((s_reg_GX_MASTER_BRIGHT >> 14) & 3);
  glProgramUniform1i(
      g2shaderProgramID,
      glGetUniformLocation(g2shaderProgramID, "inMasterBrightMode"),
      masterBrightMode);

  glProgramUniform1f(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "presentationWidth"),
                     singleScreen ? (float)(s_FieldViewWidth > 256 ? s_FieldViewWidth : s_WindowViewWidth) : 256.0f);
  glProgramUniform1f(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "fieldWidth"),
                     (float)s_FieldViewWidth);
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "fieldOverlayTiled"),
                     s_FieldOverlayTiled);
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "fieldDarknessMask"),
                     !isSub && s_FieldDarknessMask);
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "singleScreen"), singleScreen);
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "sideAverageTexture"),
                     s_SideAverageTexUnit);
  const bool averageSides = singleScreen && s_FieldViewWidth == SIM_NDS_SCREEN_WIDTH &&
                            s_WindowViewWidth > SIM_NDS_SCREEN_WIDTH && s_SideAverageReady;
  glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "sideAverageValid"), averageSides);
  glBindVertexArray(g2vertexArray);
  glBindBuffer(GL_ARRAY_BUFFER, g2vertexBuffer);
  if (averageSides) {
    GLint viewport[4];
    GLint framebuffer;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0 + s_SideAverageTexUnit);
    // Avoid sampling the texture attached to the reduction framebuffer.
    glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
    glBindFramebuffer(GL_FRAMEBUFFER, s_SideAverageFramebuffer);
    glViewport(0, 0, 256, 256);
    glProgramUniform1f(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "presentationWidth"), 256.0f);
    glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "averagePass"), TRUE);
    DrawScreenQuad();
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (blendEnabled) {
      glEnable(GL_BLEND);
    }
    glBindTexture(GL_TEXTURE_2D, s_SideAverageTexture);
    glGenerateMipmap(GL_TEXTURE_2D);
    glProgramUniform1i(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "averagePass"), FALSE);
    glProgramUniform1f(g2shaderProgramID, glGetUniformLocation(g2shaderProgramID, "presentationWidth"),
                       (float)s_WindowViewWidth);
  }
  DrawScreenQuad();

  // Clear out the textures
  for (int i = 0; i < 4; i++) {
    glActiveTexture(GL_TEXTURE0 + s_bgTexUnits[0]);
    glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
  }

  for (int i = 0; i < 5; i++) {
    glActiveTexture(GL_TEXTURE0 + s_objTexUnits[0]);
    glBindTexture(GL_TEXTURE_2D, dummyScreenTextureId);
  }
#ifdef SDK_TRACY_ENABLE
  TracyCZoneEnd(ctx);
#endif
}

static void HandleJoystickKeyDown(int aKey) {
  if (aKey == s_SIM_config.padSettings.aJoyKey) {
    // A Button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111110;
  } else if (aKey == s_SIM_config.padSettings.bJoyKey) {
    // B Button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111101;
  } else if (aKey == s_SIM_config.padSettings.xJoyKey) {
    // X Button
    *(vu16 *)HW_BUTTON_XY_BUF = *(vu16 *)HW_BUTTON_XY_BUF & 0b1111100000000000;
  } else if (aKey == s_SIM_config.padSettings.yJoyKey) {
    // y Button
    *(vu16 *)HW_BUTTON_XY_BUF = *(vu16 *)HW_BUTTON_XY_BUF & 0b1111010000000000;
  } else if (aKey == s_SIM_config.padSettings.upJoyKey) {
    // Dpad Up
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111110111111;
  } else if (aKey == s_SIM_config.padSettings.downJoyKey) {
    // Dpad Down
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111101111111;
  } else if (aKey == s_SIM_config.padSettings.leftJoyKey) {
    // Dpad Left
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111011111;
  } else if (aKey == s_SIM_config.padSettings.rightJoyKey) {
    // Dpad Right
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111101111;
  } else if (aKey == s_SIM_config.padSettings.lJoyKey) {
    // L
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111110111111111;
  } else if (aKey == s_SIM_config.padSettings.rJoyKey) {
    // R
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111011111111;
  } else if (aKey == s_SIM_config.padSettings.startJoyKey) {
    // Start button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111110111;
  } else if (aKey == s_SIM_config.padSettings.selectJoyKey) {
    // Select button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111011;
  }
#ifdef SDK_BUILD_NX
  else if (aKey == 8) {
    // Left trigger on NX toggles GUI
    SIM::GUI::Toggle();
  }
#endif
}

static void HandleJoystickKeyUp(int aKey) {
  if (aKey == s_SIM_config.padSettings.aJoyKey) {
    // A Button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000001;
  } else if (aKey == s_SIM_config.padSettings.bJoyKey) {
    // B Button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000010;
  } else if (aKey == s_SIM_config.padSettings.xJoyKey) {
    // X Button
    *(vu16 *)HW_BUTTON_XY_BUF = *(vu16 *)HW_BUTTON_XY_BUF | 0b0000010000000000;
  } else if (aKey == s_SIM_config.padSettings.yJoyKey) {
    // y Button
    *(vu16 *)HW_BUTTON_XY_BUF = *(vu16 *)HW_BUTTON_XY_BUF | 0b0000100000000000;
  } else if (aKey == s_SIM_config.padSettings.upJoyKey) {
    // Dpad Up
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000001000000;
  } else if (aKey == s_SIM_config.padSettings.downJoyKey) {
    // Dpad Down
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000010000000;
  } else if (aKey == s_SIM_config.padSettings.leftJoyKey) {
    // Dpad Left
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000100000;
  } else if (aKey == s_SIM_config.padSettings.rightJoyKey) {
    // Dpad Right
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000010000;
  } else if (aKey == s_SIM_config.padSettings.lJoyKey) {
    // L
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000001000000000;
  } else if (aKey == s_SIM_config.padSettings.rJoyKey) {
    // R
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000100000000;
  } else if (aKey == s_SIM_config.padSettings.startJoyKey) {
    // Start button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000001000;
  } else if (aKey == s_SIM_config.padSettings.selectJoyKey) {
    // Select button
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000100;
  }
}

static void Set3DViewport() {
  const u32 scale = s_SIM_config.internalResolutionScale;
  const u32 width = s_FieldViewWidth;
  glViewport((s_MaxViewWidth - width) / 2 * scale,
             (s_reg_GX_POWCNT >> 15) ? SIM_NDS_SCREEN_HEIGHT * scale : 0,
             width * scale, SIM_NDS_SCREEN_HEIGHT * scale);
}

void SIM_SetFieldOverlayTiled(BOOL enabled) {
  s_FieldOverlayTiled = enabled;
}

void SIM_SetFieldDarknessMask(BOOL enabled) {
  s_FieldDarknessMask = enabled;
}

u32 SIM_SetFieldWidescreen(BOOL enabled) {
  const u32 width = enabled && s_SIM_config.screenLayout == SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY
                        ? s_WindowViewWidth : SIM_NDS_SCREEN_WIDTH;
  if (s_FieldViewWidth != width) {
    // Geometry buffered under the previous viewport must be submitted first.
    G3SIM_FlushArray();
    G3SIM_DrawItems();
    s_FieldViewWidth = width;
    Set3DViewport();
  }
  return width;
}

void *SIM_Render(void *arg) {

  SDL_Event Event;
  char c;
  int x;
  int y;
  u8 r;
  u8 g;
  u8 b;
  u16 keys;
  u32 i;

  G3SIM_FlushArray();

  // Draw the translucent 3D stuff
  G3SIM_DrawItems();

  do {
#ifdef SDK_TRACY_ENABLE
    TracyCZone(SimRenderZone, 1);
#endif
    // SIM_Net_Process();
    if ((*((u32 *)HW_VBLANK_COUNT_BUF) % 10) == 0) {
      SIM_Net_SendBeaconIndication();
    }

    // Unbind 3D Framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glUseProgram(g2shaderProgramID);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    int windowHeight;
    int windowWidth;
    int viewHeight;
    int viewWidth;
    int viewX = 0;
    int viewY = 0;
    float aspectRatio;

    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    windowWidth = windowWidth > 0 ? windowWidth : 1;
    windowHeight = windowHeight > 0 ? windowHeight : 1;
    // Quantize to even logical pixels, keeping the 256px UI exactly centered.
    s_WindowViewWidth = (u32)fmin(448.0, fmax(256.0,
        192.0 * windowWidth / windowHeight)) & ~1u;

    // Scale the viewport to the correct aspect ratio and center it
    switch (s_SIM_config.screenLayout) {
    default:
    case SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY:
      // Present the width used by this frame's projection; resized width applies next frame.
      aspectRatio = (float)(s_FieldViewWidth > 256 ? s_FieldViewWidth : s_WindowViewWidth) / SIM_NDS_SCREEN_HEIGHT;
      break;
    case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
      aspectRatio = 256.0f / 384.0f;
      break;
    case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
      aspectRatio = 512.0f / 192.0f;
      break;
    case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
      aspectRatio = 768.0f / 384.0f;
      break;
    }

    viewWidth = windowWidth;
    viewHeight = (int)((float)windowWidth / aspectRatio);
    viewY = (windowHeight - viewHeight) / 2;
    if (viewHeight > windowHeight) {
      // Too tall to fit in the window. Scale down the width instead
      viewHeight = windowHeight;
      viewWidth = (int)((float)viewHeight * aspectRatio);
      viewX = (windowWidth - viewWidth) / 2;
      viewY = 0;
    }

    // OpenGL uses drawable pixels; mouse input remains in SDL window coordinates.
    int drawableWidth, drawableHeight;
    SDL_GL_GetDrawableSize(window, &drawableWidth, &drawableHeight);
    glViewport(viewX * drawableWidth / windowWidth, viewY * drawableHeight / windowHeight,
               viewWidth * drawableWidth / windowWidth, viewHeight * drawableHeight / windowHeight);

    // Set up the bounding box of the touch panel
    if (s_SIM_config.swapScreens) {
      switch (s_SIM_config.screenLayout) {
      default:
      case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
        s_TouchPanelCoords[0] = viewX;
        s_TouchPanelCoords[1] = viewY;
        s_TouchPanelCoords[2] = viewX + viewWidth;
        s_TouchPanelCoords[3] = viewY + (viewHeight / 2);
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
        s_TouchPanelCoords[0] = viewX;
        s_TouchPanelCoords[1] = viewY;
        s_TouchPanelCoords[2] = viewX + (viewWidth / 2);
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
        s_TouchPanelCoords[0] = viewX;
        s_TouchPanelCoords[1] = viewY;
        s_TouchPanelCoords[2] = viewX + ((viewWidth / 3) * 2);
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      }
    } else {
      switch (s_SIM_config.screenLayout) {
      default:
      case SIM_CONFIG_SCREEN_LAYOUT_VERTICAL:
        s_TouchPanelCoords[0] = viewX;
        s_TouchPanelCoords[1] = viewY + (viewHeight / 2);
        s_TouchPanelCoords[2] = viewX + viewWidth;
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_HORIZONTAL:
        s_TouchPanelCoords[0] = viewX + (viewWidth / 2);
        s_TouchPanelCoords[1] = viewY;
        s_TouchPanelCoords[2] = viewX + viewWidth;
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      case SIM_CONFIG_SCREEN_LAYOUT_LARGE:
        s_TouchPanelCoords[0] = viewX + ((viewWidth / 3) * 2);
        s_TouchPanelCoords[1] = viewY + (viewHeight / 2);
        s_TouchPanelCoords[2] = viewX + viewWidth;
        s_TouchPanelCoords[3] = viewY + viewHeight;
        break;
      }
    }

    // Check Joysticks
    if (s_SIM_SDLJoystick == NULL && SDL_NumJoysticks() >= 1) {
      s_SIM_SDLJoystick = SDL_JoystickOpen(0);
    }

    while (SDL_PollEvent(&Event)) {
      SIM::GUI::ProcessEvent(&Event);

      if (Event.type == SDL_WINDOWEVENT) {
        switch (Event.window.event) {
        case SDL_WINDOWEVENT_CLOSE:
          exit(0);
          break;
        }
      }
      if (Event.type == SDL_KEYDOWN) {
        int keyRead = Event.key.keysym.sym;
        if (keyRead == s_SIM_config.padSettings.aKey) {
          // A Button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111110;
        } else if (keyRead == s_SIM_config.padSettings.bKey) {
          // B Button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111101;
        } else if (keyRead == s_SIM_config.padSettings.xKey) {
          // X Button
          *(vu16 *)HW_BUTTON_XY_BUF =
              *(vu16 *)HW_BUTTON_XY_BUF & 0b1111100000000000;
        } else if (keyRead == s_SIM_config.padSettings.yKey) {
          // y Button
          *(vu16 *)HW_BUTTON_XY_BUF =
              *(vu16 *)HW_BUTTON_XY_BUF & 0b1111010000000000;
        } else if (keyRead == s_SIM_config.padSettings.upKey) {
          // Dpad Up
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111110111111;
        } else if (keyRead == s_SIM_config.padSettings.downKey) {
          // Dpad Down
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111101111111;
        } else if (keyRead == s_SIM_config.padSettings.leftKey) {
          // Dpad Left
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111011111;
        } else if (keyRead == s_SIM_config.padSettings.rightKey) {
          // Dpad Right
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111101111;
        } else if (keyRead == s_SIM_config.padSettings.lKey) {
          // L
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111110111111111;
        } else if (keyRead == s_SIM_config.padSettings.rKey) {
          // R
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111011111111;
        } else if (keyRead == s_SIM_config.padSettings.startKey) {
          // Start button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111110111;
        } else if (keyRead == s_SIM_config.padSettings.selectKey) {
          // Select button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT & 0b1111111111111011;
        } else if (keyRead == s_SIM_config.padSettings.guiKey) {
          // Toggle Debug GUI
          SIM::GUI::Toggle();
        }
      }
      if (Event.type == SDL_KEYUP) {
        int keyRead = Event.key.keysym.sym;
        if (keyRead == s_SIM_config.padSettings.aKey) {
          // A Button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000001;
        } else if (keyRead == s_SIM_config.padSettings.bKey) {
          // B Button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000010;
        } else if (keyRead == s_SIM_config.padSettings.xKey) {
          // X Button
          *(vu16 *)HW_BUTTON_XY_BUF =
              *(vu16 *)HW_BUTTON_XY_BUF | 0b0000010000000000;
        } else if (keyRead == s_SIM_config.padSettings.yKey) {
          // y Button
          *(vu16 *)HW_BUTTON_XY_BUF =
              *(vu16 *)HW_BUTTON_XY_BUF | 0b0000100000000000;
        } else if (keyRead == s_SIM_config.padSettings.upKey) {
          // Dpad Up
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000001000000;
        } else if (keyRead == s_SIM_config.padSettings.downKey) {
          // Dpad Down
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000010000000;
        } else if (keyRead == s_SIM_config.padSettings.leftKey) {
          // Dpad Left
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000100000;
        } else if (keyRead == s_SIM_config.padSettings.rightKey) {
          // Dpad Right
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000010000;
        } else if (keyRead == s_SIM_config.padSettings.lKey) {
          // L
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000001000000000;
        } else if (keyRead == s_SIM_config.padSettings.rKey) {
          // R
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000100000000;
        } else if (keyRead == s_SIM_config.padSettings.startKey) {
          // Start button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000001000;
        } else if (keyRead == s_SIM_config.padSettings.selectKey) {
          // Select button
          s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT | 0b0000000000000100;
        }
      }
      if (Event.type == SDL_JOYBUTTONDOWN) {
        HandleJoystickKeyDown(Event.jbutton.button);
      }
      if (Event.type == SDL_JOYBUTTONUP) {
        HandleJoystickKeyUp(Event.jbutton.button);
      }
      if (Event.type == SDL_JOYHATMOTION) {
        if (Event.jhat.value == SDL_HAT_CENTERED) {
          // send keyUp event for all hats
          HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_UP);
          HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_DOWN);
          HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_LEFT);
          HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_RIGHT);
        } else {
          if (Event.jhat.value & SDL_HAT_UP) {
            HandleJoystickKeyDown(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_UP);
          } else {
            HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_UP);
          }
          if (Event.jhat.value & SDL_HAT_DOWN) {
            HandleJoystickKeyDown(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_DOWN);
          } else {
            HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_DOWN);
          }
          if (Event.jhat.value & SDL_HAT_LEFT) {
            HandleJoystickKeyDown(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_LEFT);
          } else {
            HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_LEFT);
          }
          if (Event.jhat.value & SDL_HAT_RIGHT) {
            HandleJoystickKeyDown(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_RIGHT);
          } else {
            HandleJoystickKeyUp(SIM_CONFIG_JOY_HAT_MASK | SDL_HAT_RIGHT);
          }
        }
      }
      if (Event.type == SDL_JOYAXISMOTION) {
        if (Event.jaxis.value > s_SIM_config.padSettings.joyAxisDeadzone ||
            Event.jaxis.value <
                (s_SIM_config.padSettings.joyAxisDeadzone * -1)) {
          if (Event.jaxis.value > 0) {
            HandleJoystickKeyDown(Event.jaxis.axis |
                                  SIM_CONFIG_JOY_AXIS_PLUS_MASK);
            HandleJoystickKeyUp(Event.jaxis.axis |
                                SIM_CONFIG_JOY_AXIS_MINUS_MASK);
          } else {
            HandleJoystickKeyDown(Event.jaxis.axis |
                                  SIM_CONFIG_JOY_AXIS_MINUS_MASK);
            HandleJoystickKeyUp(Event.jaxis.axis |
                                SIM_CONFIG_JOY_AXIS_PLUS_MASK);
          }
        } else {
          HandleJoystickKeyUp(Event.jaxis.axis | SIM_CONFIG_JOY_AXIS_PLUS_MASK);
          HandleJoystickKeyUp(Event.jaxis.axis |
                              SIM_CONFIG_JOY_AXIS_MINUS_MASK);
        }
      }
      if (Event.type == SDL_MOUSEBUTTONDOWN && s_SIM_config.screenLayout != SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY) {
        isMouseDown = 1;
        s_tpData.validity = 0;
        s_tpData.touch = 1;
      }
      if (Event.type == SDL_MOUSEBUTTONUP) {
        isMouseDown = 0;
        s_tpData.validity = 0;
        s_tpData.touch = 0;
      }
    }
    s_reg_PAD_KEYINPUT = s_reg_PAD_KEYINPUT;

    SDL_GetMouseState(&x, &y);

    // Convert touch coordinate to DS screen coordinates
    if (s_SIM_config.screenLayout != SIM_CONFIG_SCREEN_LAYOUT_TOP_ONLY && x >= s_TouchPanelCoords[0] && x <= s_TouchPanelCoords[2] &&
        y >= s_TouchPanelCoords[1] && y <= s_TouchPanelCoords[3]) {
      s_tpData.x = (u16)map(x, s_TouchPanelCoords[0], s_TouchPanelCoords[2], 0,
                            SIM_NDS_SCREEN_WIDTH);
      s_tpData.y = (u16)map(y, s_TouchPanelCoords[1], s_TouchPanelCoords[3], 0,
                            SIM_NDS_SCREEN_HEIGHT);
    } else {
      s_tpData.validity = 0;
      s_tpData.touch = 0;
    }

    SIM::GUI::NewFrame();
    SIM::GUI::Main();
    UpdateFastForward(window);

    memset(bgtex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);
    memset(bg0tex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);
    memset(bg1tex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);
    memset(bg2tex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);
    memset(bg3tex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);
    memset(bg0stex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);
    memset(bg1stex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);
    memset(bg2stex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);
    memset(bg3stex, 0,
           sizeof(u8) * 4 * SIM_NDS_SCREEN_WIDTH * SIM_NDS_SCREEN_HEIGHT * 2);

    // Draw MAIN engine
    DrawEngine(FALSE);

    // SUB engine
    DrawEngine(TRUE);

    // Run HBlank Interrupt
    OS_IRQTable[OS_IE_H_BLANK - 1]();

    // Run VBlank Interrupt
    OS_IRQTable[OS_IE_V_BLANK - 1]();
    s_HW_INTR_CHECK_BUF |= 1;
    *((u32 *)HW_VBLANK_COUNT_BUF) = *((u32 *)HW_VBLANK_COUNT_BUF) + 1;

    SIM::GUI::Render();

    // Calculate frametime
    struct timespec curTime;
    clock_gettime(CLOCK_MONOTONIC, &curTime);
    u64 frameNs;
    frameNs = ((curTime.tv_sec - s_SIM_lastFrameEnd.tv_sec) * 1000000000) +
              (curTime.tv_nsec - s_SIM_lastFrameEnd.tv_nsec);
    sRenderFrameTime = frameNs;

#ifdef SDK_TRACY_ENABLE
    TracyCZoneEnd(SimRenderZone);
#endif
    // Advance game frames and VBlank tasks at the configured fast-forward speed.
    if (sFastForwardActive || s_SIM_config.capFrameRate) {
      const u64 frameBudgetNs = sFastForwardActive
                                   ? (u64)(sNormalFrameBudgetNs / sEffectiveSpeed)
                                   : sNormalFrameBudgetNs;
      WaitForFrameBudget(frameNs, frameBudgetNs);
    }

    SDL_GL_SwapWindow(window);
    ApplyPendingInternalResolution();

    //Calculate full frametime (after swap)
    clock_gettime(CLOCK_MONOTONIC, &curTime);
    frameNs = ((curTime.tv_sec - s_SIM_lastFrameEnd.tv_sec) * 1000000000) +
              (curTime.tv_nsec - s_SIM_lastFrameEnd.tv_nsec);
    sFullFrameTime = frameNs;


    clock_gettime(CLOCK_MONOTONIC, &s_SIM_lastFrameEnd);
#ifdef SDK_TRACY_ENABLE
    TracyCFrameMark;
#endif

    s_numG3CommandsThisFrame = 0;
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    s_curPolygonAttr.alpha = 1.0f;

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

  } while (SIM::GUI::IsGameLogicPaused());
  // Bind 3D Framebuffer & Renderbuffer
  glBindFramebuffer(GL_FRAMEBUFFER, g3FrameBuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, g3RenderBufferId);

  // Restore the view that was selected by the game for this frame.
  Set3DViewport();

  // Activate 3D shader
  glUseProgram(g3shaderProgramID);

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  // Fog state belongs to the last visible polygon, including translucent beams.
  glEnablei(GL_BLEND, 0);
  glDisablei(GL_BLEND, 1);

  glCullFace(GL_FRONT);
  glFrontFace(GL_CW);

  return nullptr;
}

// Get the rendering time in nanoseconds
u64 SIM_GetRenderFrameTime() {
  return sRenderFrameTime;
}

// Get the full frame time (including vsync wait) in nanoseconds
u64 SIM_GetFullFrameTime() {
  return sFullFrameTime;
}

void SIM_PreRenderVBlank() { SDL_SemPost(vcountVblankSemaphore); }

void SIM_PostRenderVBlank() {
  if (G3SIM_SwapBuffersCalledThisFrame()) {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glStencilMask(0xff);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    G3SIM_ResetSwapBuffersCall();
  }
  // Reset Vcount
  s_reg_GX_VCOUNT = 0;
  SDL_SemPost(vcountResetSemaphore);
}

const char * SIM_GetLibntrGitHash() {
  return SIM_LIBNTR_GIT_HASH;
}

const char * SIM_GetProjectGitHash() {
  return SIM_PROJECT_GIT_HASH;
}

#ifdef _WIN32
int main(int argc, char *argv[]);

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hInstPrev, PSTR cmdline,
                     int cmdshow) {
  return main(__argc, __argv);
}
#endif

int main(int argc, char *argv[]) {
#ifdef SDK_BUILD_NX
  // Start up libnx sockets
  socketInitializeDefault();
#endif

#ifdef TRACY_ENABLE
  // Start up Tracy profiler
  ___tracy_startup_profiler();

  while (!TracyCIsStarted) {
    SDL_Delay(1);
  }
#endif

  // Start up debug system
  SIM_Dbg_Init();

  // Load default config
  SIM_Config_LoadDefaults(&s_SIM_config);

  // Load configuration file
  if (!SIM_Config_LoadConfigFile(&s_SIM_config)) {
    // Config file does not exist. Save config file with defaults
    SIM_Config_SaveConfigFile(&s_SIM_config);
  }

  // Initialize Nitro registers
  s_reg_PAD_KEYINPUT = 0x3ff;
  *(vu16 *)HW_BUTTON_XY_BUF = 0b0111110000000000;

  memset(s_HW_OAM, 0, sizeof(s_HW_OAM));
  memset(s_HW_DB_OAM, 0, sizeof(s_HW_DB_OAM));

  s_reg_GX_VRAMCNT_A = 0;
  s_reg_GX_VRAMCNT_B = 0;
  s_reg_GX_VRAMCNT_C = 0;
  s_reg_GX_VRAMCNT_D = 0;
  s_reg_GX_VRAMCNT_E = 0;
  s_reg_GX_VRAMCNT_F = 0;
  s_reg_GX_VRAMCNT_G = 0;
  s_reg_GX_VRAMCNT_H = 0;
  s_reg_GX_VRAMCNT_I = 0;

  WIN_Init_sTexStartAddrTable();
  WIN_Init_sTexPlttStartAddrTable();

  // Read the first 0x160 bytes of the rom and put it in the HW_ROM_HEADER_BUF
  FILE *romFile;
  romFile = fopen("header.bin", "r");

  if (romFile != NULL) {
    // Read the fields of the struct
    fread((void *)HW_ROM_HEADER_BUF, 1, 0x160, romFile);
    fclose(romFile);
  }

  CARDRomHeader *cardHeader = (CARDRomHeader *)(HW_ROM_HEADER_BUF);

#ifdef LIBNTR_CARD_GAME_CODE
  const char *gameCode = LIBNTR_CARD_GAME_CODE;
  strncpy((char *)&cardHeader->game_code, gameCode, sizeof(u32));
#endif
#ifdef LIBNTR_CARD_MAKER_CODE
  const char *makerCode = LIBNTR_CARD_MAKER_CODE;
  strncpy((char *)&cardHeader->maker_code, makerCode, sizeof(u16));
#endif

  memset((void *)HW_MAIN_MEM_SHARED, 0, 0x160);

  SIM_pxiInit();

  pthread_t thread;
  SDL_Thread *pxiThread;
  SDL_Thread *pxi7to9Thread;
  int num;
  num = 123;

  // Initialize mosaic table
  u32 i;
  u32 j;
  for (i = 0; i < 16; i++) {
    for (j = 0; j < 256; j++) {
      int offset = j % (i + 1);
      s_mosaicTable[i][j] = offset;
    }
  }

  SND_ExChannelInit();
  SND_SeqInit();

  // Zero out some shared memory
  memset(s_HW_BG_PLTT, 0, sizeof(s_HW_BG_PLTT));
  memset(s_HW_OBJ_PLTT, 0, sizeof(s_HW_OBJ_PLTT));
  memset(s_HW_DB_BG_PLTT, 0, sizeof(s_HW_DB_BG_PLTT));
  memset(s_HW_DB_OBJ_PLTT, 0, sizeof(s_HW_DB_OBJ_PLTT));
  memset(s_HW_BG_VRAM, 0, sizeof(s_HW_BG_VRAM));
  memset(s_HW_DB_BG_VRAM, 0, sizeof(s_HW_DB_BG_VRAM));
  memset(s_HW_OBJ_VRAM, 0, sizeof(s_HW_OBJ_VRAM));
  memset(s_HW_DB_OBJ_VRAM, 0, sizeof(s_HW_DB_OBJ_VRAM));

  // Zero out some registers
  s_reg_G2_BG0CNT = 0;
  s_reg_G2_BG1CNT = 0;
  s_reg_G2_BG2CNT = 0;
  s_reg_G2_BG3CNT = 0;

  s_reg_G2S_DB_BG0CNT = 0;
  s_reg_G2S_DB_BG1CNT = 0;
  s_reg_G2S_DB_BG2CNT = 0;
  s_reg_G2S_DB_BG3CNT = 0;

  // Set up OS NVRAM Config
  SIM_Config_SetOSNVRAMConfig(&s_SIM_config);

  pxiThread = SDL_CreateThread(SIM_procPxiThread, "PXI", NULL);
  pxi7to9Thread = SDL_CreateThread(SIM_procPxi7to9Thread, "PXI7to9", NULL);

  WM_sp_SIM_init();

#ifdef SDK_BUILD_WIN64
  SetConsoleOutputCP(65001);
#endif

  SIM_RenderInit(NULL);
  G3SIM_DrawInit();

// Init Sockets (only needed on windows)
#ifdef SDK_BUILD_WIN64
  WSADATA wsa;

  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    printf("Failed to initialize winsock. Error Code : %d", WSAGetLastError());
    return 1;
  }
#endif

  SIM_Net_Init();
  SIM_Net_StartThread();

  if (argc >= 2) {
    if (strcmp(argv[1], "--net-host") == 0) {
      SIM_Net_StartHost();
      SDL_SetWindowTitle(window, "Host");
    }
    if (strcmp(argv[1], "--net-client") == 0) {
      SIM_Net_StartClient(s_SIM_config.netSettings.serverIp);
      SDL_SetWindowTitle(window, "Client");
    }
  }

  NitroMain();
  return 0;
}
