#ifdef SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif
#include <nitro/fx/fx_mtx.h>
#include <nitro/types.h>

#ifndef SIM_G3_DRAW_H
#define SIM_G3_DRAW_H

#define G3_DRAW_MAX_VERTS 12000

typedef struct {
	//Position
	GLfloat x;
	GLfloat y;
	GLfloat z;
	GLfloat w;
	// NDC Z or reciprocal clip-W plane, according to the buffer mode.
	GLfloat depthPlane[3];
	//TexCoord
	GLfloat s;
	GLfloat t;
	//Color
	GLfloat r;
	GLfloat g;
	GLfloat b;
	GLfloat a;
} G3SIM_Vertex_t;

#ifdef __cplusplus
extern "C" {
#endif

void G3SIM_AddVtx(G3SIM_Vertex_t * vtx);
void G3SIM_DrawArray();
void G3SIM_DrawCleanUp();
void G3SIM_DrawInit();
void G3SIM_ClearTextureCache();
u32 G3SIM_PrewarmTexture(u32 image, u32 palette);
void G3SIM_FlushArray();
// Captured per polygon, including deferred translucent draws.
void G3SIM_SetTextureSmoothing(BOOL enabled);
void G3SIM_SetFieldEffect(int mode, float phase);
void G3SIM_SetFieldSurface(int surface);
void G3SIM_SetFieldEmitterCollection(BOOL enabled, const float worldFromClip[9], const float offset[3]);
void G3SIM_EndFieldEmitterCollection(void);
// bloom scales the emission this effect contributes to the bloom pass.
void G3SIM_SetFieldEmissiveEffect(int mode, float phase, float bloom);
// Center x, y, z, strength, radius x, z, slope x, z; see SIM_SetFieldContactShadow.
void G3SIM_SetFieldContactShadow(const float shadow[8]);
// corners packs four shore weights per tile (byte i: corner i, clockwise from -x/-z).
void G3SIM_DrawWaterTiles(const VecFx32 *tiles, const u32 *corners, int count, float phase, const MtxFx43 *view, const MtxFx44 *projection);
void G3SIM_SetWaterProfiling(BOOL enabled);
float G3SIM_GetWaterGpuMs(void);
void G3SIM_DrawItems();

#ifdef __cplusplus
}
#endif

#endif
