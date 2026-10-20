#ifndef SIM_H
#define SIM_H
#include <nitro.h>
#include <nitro/types.h>
#include <nitro/gx/gx_vramcnt.h>
#include <SDL2/SDL.h>
#include <simulator/config/sim_config.h>
#include <simulator/sim_debug.h>

#include "simulator/assert.h"

#define SIM_NDS_SCREEN_WIDTH 256
#define SIM_NDS_SCREEN_HEIGHT 192
#define SIM_FIELD_SPRITE_WIDTH 448

#define SIM_NDS_SCREEN_SCALE_FACTOR 4
#define SIM_NDS_WINDOW_SCALE_FACTOR 2

typedef struct WIN_snd_channel_info {
	u8 playing;
	u32 preLoopLen;
	u32 preLoopSamples;
	u32 loopLen;
	u32 loopSamples;
	u32 alarms_bitfield;
	u32 audio_len;
	u32 total_audio_len;
	u32 total_num_samples;
	u8 * audio_pos;
	u8 * audio_data;
	u8 inLoop;
	u32 posPreLoop;
	u32 posInLoop;
	u32 pos;
	u8 stream[500000];
	u8 resampStream[1800000];
	SDL_AudioStream * sdlStream;
	u32 streamSampleRate;
	double pitchRatio;
	u32 prevTimer;
	u8 pitchBend;
} WIN_snd_channel_info;

typedef struct WIN_snd_alarm_info {
	u32 alarmNo;
	u32 time;
	u32 period;
	u32 id;
	pthread_t thread;
	u8 isRunning;
} WIN_snd_alarm_info;

#ifdef __cplusplus
extern "C" {
#endif

void SIM_PreRenderVBlank(void);
void SIM_PostRenderVBlank(void);
void SIM_procPXI(void);
void SIM_procPXI7to9(void);
int SIM_procPxiThread( void * arg );
void SIM_PostPxiThread();
int SIM_procPxi7to9Thread( void * arg );
void SIM_PostPxi7to9Thread();
int SIM_procOSThreadThread( void * arg );
void SIM_procThread(void);
void SIM_pxiInit(void);
void * SIM_Render(void *arg);
void * SIM_RenderInit(void * arg);
void SIM_SetInternalResolutionAfterRender(int scale);
// Applied after presentation; only the 3D target is multisampled.
void SIM_Set3DAntialiasing(BOOL enabled);
int SIM_Get3DAntialiasingSamples(void);
void SIM_SetLightingCorrection(BOOL enabled);
// Analytic field primitives; zero restores ordinary model rendering.
void SIM_SetFieldEffect(int mode, float phase);
// As above; bloom weights the light an emissive material adds to the bloom.
void SIM_SetFieldEmissiveEffect(int mode, float phase, float bloom);
// The contact shadow (effect 1) under the character whose cast shadow
// (effect 23) is drawn next: its world center, radii, the slope of its plane
// (height per unit x and z) and its strength. Where they overlap, the darker
// of the two shows instead of both. Needs SIM_SetFieldWorld; zero strength
// turns it off.
void SIM_SetFieldContactShadow(const float center[3], float radiusX, float radiusZ, float slopeX, float slopeZ, float strength);
// corners packs four 0-255 shore weights per tile, clockwise from the -x/-z corner.
void SIM_DrawWaterTiles(const VecFx32 *tiles, const u32 *corners, int count, float phase, const MtxFx43 *view, const MtxFx44 *projection);
void SIM_SetWaterProfiling(BOOL enabled);
float SIM_GetWaterGpuMs(void);
u64 SIM_VisualClockNs(void);
void SIM_SetFieldEmissionDebug(BOOL enabled);
// Blurs emissive field materials into the world layer; zero disables.
void SIM_SetFieldBloom(float strength);
// Field opts in for each frame. Other scenes return to neutral shading.
void SIM_SetFieldAtmosphere(float fogR, float fogG, float fogB, float strength, float start, float end);
// Lets the field shader recover each pixel's world position this frame:
// worldFromClip maps (clip x, clip y, clip w) to world units as a row-major
// 3x3 matrix plus offset; camera is the camera's world position; time (in
// seconds) animates weather. Perspective views only.
void SIM_SetFieldWorld(const float worldFromClip[9], const float offset[3], const float camera[3], float time);
// Water surfaces (field effect 27) mirror lamps, windows and signs, on screen
// or not, as the previous field frame drew them. Call once per field frame,
// before drawing it, after SIM_SetFieldWorld; FALSE stops it.
void SIM_SetFieldWaterReflection(BOOL enabled);
// Rain on world surfaces for this field frame (0: dry). Needs SIM_SetFieldWorld.
void SIM_SetFieldWetness(float wetness);
// Drifting mist that hugs the ground below a world height (base) and thins
// out over depth units above it. Needs SIM_SetFieldWorld.
void SIM_SetFieldMist(float r, float g, float b, float strength, float base, float depth);
// Brackets drawing world geometry (terrain and buildings), which weather
// such as rain affects, as opposed to sprites and overlays.
void SIM_SetFieldSurface(BOOL surface);
// Field color grade, applied to the 3D world before fog and bloom; emissive
// materials bypass it. Identity: tints of 1, exposure, saturation, contrast 1.
void SIM_SetFieldGrade(const float shadows[3], const float highlights[3], float exposure, float saturation, float contrast);
int SIM_runNitroOSThread(void * thread);
void * SIM_sndAlarm(void * arg);
void SIM_u16ToRGB( u16 in, u8 * r, u8 *g, u8 * b );
SIM_config_type * SIM_GetConfigPtr();
// Field rendering opts in each logic frame; all other scenes use 4:3.
// Returns the logical width for the matching projection (256..448).
u32 SIM_SetFieldWidescreen(BOOL enabled);
#if defined(SDK_BUILD_LINUX) || defined(SDK_BUILD_WIN64)
void SIM_ResetVirtualTextureAddresses(void);
void *SIM_GetExtendedTextureStorage(u32 address, u32 size);
u32 SIM_EncodeTextureAddress(u32 parameter, u32 baseAddress);
u32 SIM_DecodeTextureAddress(u32 *parameter);
#endif
// Repeat seamless weather BG2 textures across the wide field viewport.
void SIM_SetFieldOverlayTiled(BOOL enabled);
// Stretch a main-engine BG screen mask across the field viewport; -1 disables.
// Sprites and the 3D scene keep their original coordinates.
void SIM_SetFieldTransitionMask(int layer);
// Carry a main-engine BG layer's edge dots on into the wide field viewport's
// sides on screen rows [firstRow, endRow), as for a band across the screen
// (the BW2 battle's message window). layer -1 turns it off.
void SIM_SetFieldEdgeRows(int layer, int firstRow, int endRow);
// Extend the opaque cave mask across the wide field viewport.
void SIM_SetFieldDarknessMask(BOOL enabled);
// Extend the HM cut-in band without stretching its sprites.
void SIM_SetFieldMoveCutIn(BOOL enabled);
// Scale screen-fade window masks to the current viewport, per DS engine.
void SIM_SetScreenFadeWindows(BOOL main, BOOL sub);
// Register/unregister a MAIN OBJ VRAM range containing seamless field particles.
void SIM_SetFieldTiledSpriteRange(u32 address, u32 size, BOOL enabled);
// Continue selected field sprites past the native screen edges without repeating.
void SIM_SetFieldWideSpriteRange(u32 address, u32 size, BOOL enabled);
// Continue league banner sprites into the wide viewport using their 16-dot,
// 12-color palette ramp. Border colors and transparent pixels are preserved.
void SIM_SetFieldSpriteEdgeRange(u32 address, u32 size, BOOL enabled);
u64 SIM_GetRenderFrameTime();
// Session-only, nonblocking whole-frame GPU timing on the render context.
void SIM_SetRenderGpuProfiling(BOOL enabled);
float SIM_GetRenderGpuMs(void); // -1 until a result is available.
int SIM_GetRenderGpuSampleAge(void);
const char *SIM_GetGraphicsRenderer(void);
u64 SIM_GetFullFrameTime();
const char * SIM_GetLibntrGitHash();
const char * SIM_GetProjectGitHash();
SDL_Window * SIM_GetSDLWindow();

extern GXVRamTex s_SIM_GXVRamTex;
extern GXVRamTexPltt s_SIM_GXVRamTexPltt;
extern GXVRamBGExtPltt s_SIM_GXBgExtPltt;
extern GXVRamSubBGExtPltt s_SIM_GXSubBgExtPltt;

extern SNDSharedWork * win_SNDi_SharedWork;

extern void * win_snd_channel_bufs[32];
extern WIN_snd_channel_info win_snd_channels[32];
extern WIN_snd_channel_info win_sndi_channels[16];
extern WIN_snd_alarm_info win_snd_alarms[32];

#ifdef __cplusplus
}
#endif

#endif
