#include <simulator/sim.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <nitro.h>
#include <nitro/gx/g3.h>
#include <nitro/gx/gx_load.h>
#include <nitro/gx/gx_vramcnt.h>
#include <simulator/assert.h>
#include <simulator/g3_draw.h>
#include <simulator/g3_handler.h>
#include <simulator/sim_crc32.h>
#include <simulator/sim_frame_profile.h>
#include <simulator/sim_polygon_stencil.hpp>
#include <stddef.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#ifdef SDK_BUILD_LINUX
#include <signal.h>
#endif

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

#define G3_DRAW_MAX_ITEMS 1000

#define getbit(x,n) ( ( (x) >> (n) ) & 1 )

struct G3Texture {
    GLuint id = 0;
    bool smooth = false;
    u32 width, height, format, color0, refs = 0;
    u64 used;
    std::vector<u8> source, palette;
    // The color its brightest texels glow in, for water reflections.
    float glow[3] = { 1, 1, 1 };
};
static std::unordered_multimap<u64, G3Texture *> sGPUTextures;
static u64 sGPUBytes, sSourceBytes, sTextureUse, sUploadedBytes;
static u64 G3SIM_TextureBudget() {
    const char *value = getenv("PLATINUM_GPU_CACHE_MB");
    unsigned long mb = value ? strtoul(value, nullptr, 10) : 64;
    if (mb < 16 || mb > 512) mb = 64;
    return mb * 1024 * 1024;
}
static void G3SIM_TrimTextures(G3Texture *keep) {
    const u64 budget = G3SIM_TextureBudget();
    const char *value = getenv("PLATINUM_CACHE_MB");
    unsigned long cpuMB = value ? strtoul(value, nullptr, 10) : 256;
    if (cpuMB < 128 || cpuMB > 1024) cpuMB = 256;
    // Encoded copies used to validate direct VRAM writes share the CPU ceiling.
    const u64 sourceBudget = cpuMB * 1024 * 1024 / 4;
    while (sGPUBytes > budget || sSourceBytes > sourceBudget) {
        auto oldest = sGPUTextures.end();
        for (auto i = sGPUTextures.begin(); i != sGPUTextures.end(); ++i)
            if (i->second != keep && !i->second->refs &&
                (oldest == sGPUTextures.end() || i->second->used < oldest->second->used)) oldest = i;
        if (oldest == sGPUTextures.end()) break; // Queued versions stay pinned until submission.
        G3Texture *entry = oldest->second;
        sGPUBytes -= 4 * entry->width * entry->height;
        sSourceBytes -= entry->source.size() + entry->palette.size();
        glDeleteTextures(1, &entry->id);
        delete entry;
        sGPUTextures.erase(oldest);
    }
    SIM_ProfileSet(SIM_PROFILE_GPU_CACHE_BYTES, sGPUBytes);
    SIM_ProfileSet(SIM_PROFILE_TEXTURE_SOURCE_BYTES, sSourceBytes);
}

static unsigned sDrawEpoch;
static bool sTextureSmoothing;

void G3SIM_SetTextureSmoothing(BOOL enabled)
{
    if (sTextureSmoothing == (enabled != FALSE)) return;
    G3SIM_FlushArray();
    sTextureSmoothing = enabled != FALSE;
}

// The texture cache is shared by immediate and deferred polygons. Apply the
// requesting polygon's filter each time a cached texture changes between them.
static void ApplyTextureSmoothing(G3Texture *texture, bool enabled)
{
    if (!texture || texture->smooth == enabled) return;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, enabled ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, enabled ? GL_LINEAR : GL_NEAREST);
    texture->smooth = enabled;
}

static int sFieldEffectMode;
static float sFieldEffectPhase;
static float sFieldEffectBloom = 1;
static float sFieldContactShadow[8];
// World geometry (terrain, buildings) as opposed to sprites and overlays;
// weather such as rain wets only these.
static int sFieldSurface;

// Water reflections. While a field frame draws, polygons with an emissive
// field effect (lamps, windows, signs) and water surfaces (mode 27) are
// recorded in world space, on screen or not. The next frame's water mirrors
// those lights; world positions keep that one frame of delay invisible.
struct FieldEmitter {
    float position[3], color[3], strength, weight;
};
struct FieldWater {
    float min[2], max[2], y;
};
extern GLuint g3shaderProgramID;
static bool sCollectEmitters, sCollectedLastFrame;
static double sWorldFromClip[9], sWorldFromClipOffset[3];
static std::vector<FieldEmitter> sPendingEmitters, sEmitters;
static std::vector<FieldWater> sWater;
static bool EmissiveFieldMode(int mode)
{
    return mode == 9 || mode == 10 || mode == 13 || mode == 14 || mode == 16 || mode == 18 || mode == 20;
}
static void ToWorld(const G3SIM_FxVtx_t &v, double world[3])
{
    double clip[3] = { v.x / 4096.0, v.y / 4096.0, v.w / 4096.0 };
    for (int i = 0; i < 3; i++) {
        world[i] = sWorldFromClipOffset[i];
        for (int k = 0; k < 3; k++) world[i] += sWorldFromClip[i * 3 + k] * clip[k];
    }
}
// Hands the water shader the reflections of the lights nearest the middle of
// the screen (on screen or just off it). Each sits on the water in front of
// its light: right below it, or, for a lamp on a bank within 12 units of the
// water, 9 units onto the nearest water. It depends only on the light, not on
// the camera, so it stays put as the camera moves. Lights set back from the
// water, such as on a building beside it, do not reflect.
static void UploadReflections()
{
    enum { MAX_UPLOAD = 48 };
    // World = M * (clip x, clip y, clip w) + offset; invert it to project.
    const double *m = sWorldFromClip;
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    double inverse[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, d = (i + 2) % 3;
            inverse[i * 3 + j] = (m[a * 3 + c] * m[b * 3 + d] - m[a * 3 + d] * m[b * 3 + c]) / det;
        }
    // Reflections stretch down the screen: from the middle of the view
    // towards the camera, along the ground.
    float down[2] = { (float)-m[2], (float)-m[8] };
    float downLength = std::sqrt(down[0] * down[0] + down[1] * down[1]);
    if (downLength < 1e-6f) {
        down[0] = 0;
        down[1] = 1;
    } else {
        down[0] /= downLength;
        down[1] /= downLength;
    }
    std::vector<std::pair<float, int>> order;
    for (int i = 0; i < (int)sEmitters.size(); i++) {
        double rel[3], clip[3] = { };
        for (int k = 0; k < 3; k++) rel[k] = sEmitters[i].position[k] - sWorldFromClipOffset[k];
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) clip[r] += inverse[r * 3 + k] * rel[k];
        if (clip[2] <= 0) continue;
        double x = clip[0] / clip[2], y = clip[1] / clip[2];
        if (std::fabs(x) > 2.5 || std::fabs(y) > 2.5) continue;
        order.emplace_back((float)(x * x + y * y), i);
    }
    std::sort(order.begin(), order.end());
    // Each reflection: its spot on the water (x, z), the light's height above
    // the water, and its strength.
    float positions[MAX_UPLOAD * 4] = { }, colors[MAX_UPLOAD * 3] = { };
    int count = 0;
    for (int n = 0; n < (int)order.size() && count < MAX_UPLOAD; n++) {
        const FieldEmitter &e = sEmitters[order[n].second];
        float best = 12 * 12, spot[2] = { }, waterY = 0;
        bool found = false;
        for (const FieldWater &w : sWater) {
            if (e.position[1] - w.y < 1) continue;
            float ex = std::min(std::max(e.position[0], w.min[0]), w.max[0]);
            float ez = std::min(std::max(e.position[2], w.min[1]), w.max[1]);
            float d = (ex - e.position[0]) * (ex - e.position[0]) + (ez - e.position[2]) * (ez - e.position[2]);
            if (d > best || (found && d == best)) continue;
            float inset[2] = { std::min(9.0f, (w.max[0] - w.min[0]) / 2), std::min(9.0f, (w.max[1] - w.min[1]) / 2) };
            spot[0] = e.position[0] < w.min[0] ? w.min[0] + inset[0] : e.position[0] > w.max[0] ? w.max[0] - inset[0] : e.position[0];
            spot[1] = e.position[2] < w.min[1] ? w.min[1] + inset[1] : e.position[2] > w.max[1] ? w.max[1] - inset[1] : e.position[2];
            best = d;
            waterY = w.y;
            found = true;
        }
        if (!found) continue;
        positions[count * 4 + 0] = spot[0];
        positions[count * 4 + 1] = spot[1];
        positions[count * 4 + 2] = e.position[1] - waterY;
        positions[count * 4 + 3] = e.strength;
        for (int c = 0; c < 3; c++) colors[count * 3 + c] = e.color[c];
        count++;
    }
    glProgramUniform4fv(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "emitterPosition"), MAX_UPLOAD, positions);
    glProgramUniform3fv(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "emitterColor"), MAX_UPLOAD, colors);
    glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "emitterCount"), count);
    glProgramUniform2f(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "reflectionDown"), down[0], down[1]);
}
void G3SIM_SetFieldEmitterCollection(BOOL enabled, const float worldFromClip[9], const float offset[3])
{
    sPendingEmitters.clear();
    if (enabled) {
        for (int i = 0; i < 9; i++) sWorldFromClip[i] = worldFromClip[i];
        for (int i = 0; i < 3; i++) sWorldFromClipOffset[i] = offset[i];
        // Last frame's lights and water, from this frame's camera.
        if (sCollectedLastFrame) UploadReflections();
    }
    sCollectEmitters = enabled;
    sCollectedLastFrame = false;
    sEmitters.clear();
    sWater.clear();
}
// Every frame stops collecting; field frames start it again.
void G3SIM_EndFieldEmitterCollection()
{
    sCollectedLastFrame = sCollectEmitters;
    sCollectEmitters = false;
}
void G3SIM_CollectEmitterPolygon(const G3SIM_FxVtx_t *verts, int count)
{
    if (!sCollectEmitters)
        return;
    if (sFieldEffectMode == 27) {
        // A water surface, by its extent; only level ones.
        if (sWater.size() >= 2048) return;
        FieldWater w = { { 1e9f, 1e9f }, { -1e9f, -1e9f }, 0 };
        float low = 1e9f, high = -1e9f;
        for (int i = 0; i < count; i++) {
            double world[3];
            ToWorld(verts[i], world);
            w.min[0] = std::min(w.min[0], (float)world[0]);
            w.max[0] = std::max(w.max[0], (float)world[0]);
            w.min[1] = std::min(w.min[1], (float)world[2]);
            w.max[1] = std::max(w.max[1], (float)world[2]);
            low = std::min(low, (float)world[1]);
            high = std::max(high, (float)world[1]);
        }
        if (high - low > 2) return;
        w.y = (low + high) / 2;
        sWater.push_back(w);
        return;
    }
    if (!EmissiveFieldMode(sFieldEffectMode) || sFieldEffectPhase <= 0.01f || sPendingEmitters.size() >= 4096)
        return;
    FieldEmitter emitter = { };
    for (int i = 0; i < count; i++) {
        double world[3];
        ToWorld(verts[i], world);
        for (int c = 0; c < 3; c++) emitter.position[c] += (float)world[c] / count;
        emitter.color[0] += verts[i].r / count;
        emitter.color[1] += verts[i].g / count;
        emitter.color[2] += verts[i].b / count;
    }
    emitter.strength = sFieldEffectPhase * sFieldEffectBloom;
    emitter.weight = 1;
    sPendingEmitters.push_back(emitter);
}
// Gives the pending polygons their batch's glow color and merges them into
// clusters about one tile across.
static void CommitEmitters(const G3Texture *texture)
{
    if (sPendingEmitters.empty())
        return;
    float color[3];
    if (sFieldEffectMode == 9 || sFieldEffectMode == 16) {
        // Lit windows glow warm whatever their glass color.
        color[0] = 1; color[1] = 0.78f; color[2] = 0.38f;
    }
    for (FieldEmitter &pending : sPendingEmitters) {
        if (sFieldEffectMode != 9 && sFieldEffectMode != 16) {
            for (int c = 0; c < 3; c++) color[c] = texture ? texture->glow[c] : pending.color[c];
        }
        FieldEmitter *cluster = nullptr;
        for (FieldEmitter &e : sEmitters) {
            if (std::fabs(e.position[0] - pending.position[0]) < 12 && std::fabs(e.position[2] - pending.position[2]) < 12
                && std::fabs(e.position[1] - pending.position[1]) < 24) {
                cluster = &e;
                break;
            }
        }
        if (!cluster) {
            if (sEmitters.size() >= 512) continue;
            FieldEmitter e = pending;
            for (int c = 0; c < 3; c++) e.color[c] = color[c];
            sEmitters.push_back(e);
            continue;
        }
        float w = cluster->weight;
        for (int c = 0; c < 3; c++) {
            cluster->position[c] = (cluster->position[c] * w + pending.position[c]) / (w + 1);
            cluster->color[c] = (cluster->color[c] * w + color[c]) / (w + 1);
        }
        cluster->strength = std::max(cluster->strength, pending.strength);
        cluster->weight = w + 1;
    }
    sPendingEmitters.clear();
}
extern GLuint g3shaderProgramID;
void G3SIM_SetFieldEffect(int mode, float phase) {
    G3SIM_SetFieldEmissiveEffect(mode, phase, 1);
}
void G3SIM_SetFieldSurface(int surface) {
    if (surface == sFieldSurface)
        return;
    G3SIM_FlushArray();
    sFieldSurface = surface;
}
void G3SIM_SetFieldEmissiveEffect(int mode, float phase, float bloom) {
    if (mode == sFieldEffectMode && phase == sFieldEffectPhase && bloom == sFieldEffectBloom)
        return;
    G3SIM_FlushArray();
    sFieldEffectMode = mode;
    sFieldEffectPhase = phase;
    sFieldEffectBloom = bloom;
}
void G3SIM_SetFieldContactShadow(const float shadow[8]) {
    if (memcmp(shadow, sFieldContactShadow, sizeof(sFieldContactShadow)) == 0)
        return;
    G3SIM_FlushArray();
    memcpy(sFieldContactShadow, shadow, sizeof(sFieldContactShadow));
}
static void ApplyFieldEffect(int mode, float phase, float bloom, int surface, const float contactShadow[8]) {
    static GLuint program;
    static unsigned epoch;
    static GLint modeLocation, phaseLocation, bloomLocation, surfaceLocation, contactLocation;
    static int appliedMode = -1, appliedSurface = -1;
    static float appliedPhase = -1, appliedBloom = -1, appliedContact[8] = { -1 };
    if (program != g3shaderProgramID || epoch != sDrawEpoch) {
        epoch = sDrawEpoch;
        program = g3shaderProgramID;
        modeLocation = glGetUniformLocation(program, "fieldEffectMode");
        phaseLocation = glGetUniformLocation(program, "fieldEffectPhase");
        bloomLocation = glGetUniformLocation(program, "fieldEmissionWeight");
        surfaceLocation = glGetUniformLocation(program, "fieldSurface");
        contactLocation = glGetUniformLocation(program, "fieldContactShadow");
        appliedMode = -1;
        appliedContact[3] = -1;
        appliedSurface = -1;
        appliedPhase = -1;
        appliedBloom = -1;
    }
    // Opaque and deferred draws share this cache; effect changes still apply
    // in their actual submission order, rather than the model traversal order.
    if (appliedMode != mode) {
        glProgramUniform1i(program, modeLocation, mode);
        appliedMode = mode;
    }
    if (appliedPhase != phase) {
        glProgramUniform1f(program, phaseLocation, phase);
        appliedPhase = phase;
    }
    if (appliedBloom != bloom) {
        glProgramUniform1f(program, bloomLocation, bloom);
        appliedBloom = bloom;
    }
    if (appliedSurface != surface) {
        glProgramUniform1i(program, surfaceLocation, surface);
        appliedSurface = surface;
    }
    if (memcmp(appliedContact, contactShadow, sizeof(appliedContact)) != 0) {
        glProgramUniform4fv(program, contactLocation, 2, contactShadow);
        memcpy(appliedContact, contactShadow, sizeof(appliedContact));
    }
}

extern G3SIM_TexImageParam_t s_texImageParam;
static void G3SIM_ApplyPolygonRasterState(const G3SIM_PolygonAttr_t &polygon);
struct WaterTimer {
    GLuint start, end;
    unsigned frame;
};
static bool sWaterProfiling;
static unsigned sWaterFrame;
static float sWaterGpuMs;
static std::deque<WaterTimer> sWaterPending;
static std::vector<WaterTimer> sWaterFree;
static unsigned sWaterResultFrame;
static double sWaterResultNs;
void G3SIM_SetWaterProfiling(BOOL enabled)
{
    sWaterProfiling = enabled != FALSE;
}
float G3SIM_GetWaterGpuMs(void)
{
    return sWaterGpuMs;
}
static void PollWaterTimers()
{
    while (!sWaterPending.empty()) {
        WaterTimer timer = sWaterPending.front();
        GLint ready = 0;
        glGetQueryObjectiv(timer.end, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready)
            break;
        GLuint64 start, end;
        glGetQueryObjectui64v(timer.start, GL_QUERY_RESULT, &start);
        glGetQueryObjectui64v(timer.end, GL_QUERY_RESULT, &end);
        if (timer.frame != sWaterResultFrame) {
            sWaterResultNs = 0;
            sWaterResultFrame = timer.frame;
        }
        sWaterResultNs += end - start;
        sWaterPending.pop_front();
        sWaterFree.push_back(timer);
        if (sWaterPending.empty() || sWaterPending.front().frame != timer.frame)
            sWaterGpuMs = (float)(sWaterResultNs / 1000000.0);
    }
}

void G3SIM_DrawWaterTiles(const VecFx32 *tiles, const u32 *corners, int count, float phase, const MtxFx43 *view, const MtxFx44 *projection)
{
    G3SIM_FlushArray();
    PollWaterTimers();
    ++sWaterFrame;
    const auto savedPolygon = s_curPolygonAttr;
    const auto savedTexture = s_texImageParam;
    G3SIM_TexImageParam(0);
    // Fog enabled: the overlay must carry the scene's fog, not erase it.
    G3SIM_PolygonAttr((47U << 24) | (30U << 16) | (1U << 15) | (3U << 6));
    double clipMatrix[4][4] = { };
    for (int j = 0; j < 4; j++)
        for (int axis = 0; axis < 4; axis++) {
            for (int k = 0; k < 3; k++)
                clipMatrix[j][axis] += view->m[j][k] / 4096.0 * projection->m[k][axis] / 4096.0;
            if (j == 3)
                clipMatrix[j][axis] += projection->m[3][axis] / 4096.0;
        }
    // One continuous pattern: texture coordinates are world positions in tile
    // units, so neighbouring tiles join without seams. Each corner carries how
    // much open water surrounds it, fading the effect out towards the shore.
    G3SIM_SetFieldEffect(3, phase);
    for (int index = 0; index < count; index++) {
        G3SIM_FxVtx_t quad[4] = { };
        for (int corner = 0; corner < 4; corner++) {
            bool right = corner == 1 || corner == 2, bottom = corner >= 2;
            double world[4] = { tiles[index].x / 4096.0 + (right ? 8 : -8), tiles[index].y / 4096.0, tiles[index].z / 4096.0 + (bottom ? 8 : -8), 1 };
            for (int axis = 0; axis < 4; axis++) {
                double value = 0;
                for (int j = 0; j < 4; j++)
                    value += world[j] * clipMatrix[j][axis];
                quad[corner].nums[axis] = (fx32)std::llround(value * FX32_ONE);
            }
            quad[corner].s = (float)(world[0] / 16);
            quad[corner].t = (float)(world[2] / 16);
            quad[corner].r = 1;
            quad[corner].g = 1;
            quad[corner].b = 1;
            quad[corner].a = ((corners[index] >> (corner * 8)) & 255) / 255.0f;
        }
        G3SIM_SubmitPolygon(quad, 4);
    }
    G3SIM_SetFieldEffect(0, 0);
    s_curPolygonAttr = savedPolygon;
    s_texImageParam = savedTexture;
    G3SIM_ApplyPolygonRasterState(savedPolygon);
}

// Deferred polygons must retain their own fog state. Flashlight models disable
// fog, so inheriting the last submitted polygon's uniforms lights the whole gym.
typedef struct {
	int enabled;
	u32 control;
	u32 color;
	u32 offset;
	u8 density[32];
} g3_fog_state_t;

typedef struct {
	u8 useWBuffer;
	GLuint textureId;
	G3Texture *texture;
	bool smoothTexture;
	G3SIM_PolygonAttr_t polygonAttr;
	g3_fog_state_t fog;
    int fieldEffectMode;
    float fieldEffectPhase;
    float fieldEffectBloom;
    float fieldContactShadow[8];
    int fieldSurface;
	G3SIM_TexImageParam_t texImageParam;
	G3SIM_Vertex_t * verts;
	u32 vertsCount;
} g3_draw_item_t;

G3SIM_Vertex_t s_G3DrawVerts[G3_DRAW_MAX_VERTS];



//The item list is used for drawing translucent things sorted by Z position
static g3_draw_item_t s_G3DrawItemList[G3_DRAW_MAX_ITEMS];
static u32 s_G3DrawItemListCount = 0;

static u32 s_G3DrawCurVertIdx = 0;
static GLuint s_G3DrawVertexArray;
static GLuint s_G3DrawVertexBuffer;
// Append draws to unused storage. Rewriting offset zero for every polygon can
// make the driver wait for the GPU's previous draw, especially at high resolution.
static constexpr int kStreamVertexCapacity = G3_DRAW_MAX_VERTS * 8;
static int sStreamVertexCursor;
static GLint UploadDrawVertices(const G3SIM_Vertex_t *vertices, int count)
{
    SIM_assert(count <= kStreamVertexCapacity);
    glBindVertexArray(s_G3DrawVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
    if (sStreamVertexCursor + count > kStreamVertexCapacity) {
        // Orphan exhausted storage; queued draws retain the old backing store.
        glBufferData(GL_ARRAY_BUFFER, sizeof(G3SIM_Vertex_t) * kStreamVertexCapacity, nullptr, GL_STREAM_DRAW);
        sStreamVertexCursor = 0;
    }
    GLint first = sStreamVertexCursor;
    glBufferSubData(GL_ARRAY_BUFFER, sizeof(G3SIM_Vertex_t) * first,
        sizeof(G3SIM_Vertex_t) * count, vertices);
    sStreamVertexCursor += count;
    return first;
}

extern G3SIM_TexImageParam_t s_texImageParam;
extern G3SIM_PolygonAttr_t s_curPolygonAttr;
extern GLuint g3shaderProgramID;
extern u8 s_SIM_useWBuffer;
extern u8 s_SIM_g3tex[4*1024*1024];
extern GXVRamTex s_SIM_GXVRamTex;
extern GXVRamTexPltt s_SIM_GXVRamTexPltt;


// The CPU cache supplies decoded pixels. Allocate upload targets only for
// dimensions actually used, instead of preallocating the old fixed-size pool.
// Deferred draws still own independent copies of their decoded textures.
static GLuint SIM_GetTextureID() {
    static std::unordered_map<u32, GLuint> targets;
    const u32 width = s_texImageParam.textureSSize;
    const u32 height = s_texImageParam.textureTSize;
    const u32 key = (width << 16) | height;
    GLuint &id = targets[key];
    glActiveTexture(GL_TEXTURE0);
    if (!id) {
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, id);
    return id;
}

static void* getTextureVramBank();
static void* getTexPlttVramBank();

static void* getTextureVramBank()
{
	void * vramBank = NULL;
	switch( s_SIM_GXVRamTex ){
		case GX_VRAM_TEX_0_A:
		case GX_VRAM_TEX_01_AB:
		case GX_VRAM_TEX_012_ABC:
		case GX_VRAM_TEX_0123_ABCD:
		case GX_VRAM_TEX_01_AC:
		case GX_VRAM_TEX_01_AD:
		case GX_VRAM_TEX_012_ABD:
		case GX_VRAM_TEX_012_ACD:
			vramBank = (void *)HW_LCDC_VRAM_A;
			break;
		case GX_VRAM_TEX_0_B:
		case GX_VRAM_TEX_01_BC:
		case GX_VRAM_TEX_012_BCD:
		case GX_VRAM_TEX_01_BD:
			vramBank = (void *)HW_LCDC_VRAM_B;
			break;
		case GX_VRAM_TEX_0_C:
		case GX_VRAM_TEX_01_CD:
			vramBank = (void *)HW_LCDC_VRAM_C;
			break;
		case GX_VRAM_TEX_0_D:
			vramBank = (void *)HW_LCDC_VRAM_A;
			break;
		default:
			//?
			break;
	}
	
	return vramBank;
}

static void* getTexPlttVramBank()
{
	void * vramBank = NULL;
	switch( s_SIM_GXVRamTexPltt ) {
		case GX_VRAM_TEXPLTT_NONE:
		case GX_VRAM_TEXPLTT_0123_E:
		case GX_VRAM_TEXPLTT_01234_EF:
		case GX_VRAM_TEXPLTT_012345_EFG:
			vramBank = (void *)HW_LCDC_VRAM_E;
			break;
		case GX_VRAM_TEXPLTT_0_F:
		case GX_VRAM_TEXPLTT_01_FG:
			vramBank = (void *)HW_LCDC_VRAM_F;
			break;
		case GX_VRAM_TEXPLTT_0_G:
			vramBank = (void *)HW_LCDC_VRAM_G;
			break;
	}
	
	return vramBank;
}

void G3SIM_AddVtx(G3SIM_Vertex_t * vtx)
{
	memcpy(&s_G3DrawVerts[s_G3DrawCurVertIdx], vtx, sizeof(G3SIM_Vertex_t));

	s_G3DrawCurVertIdx = s_G3DrawCurVertIdx + 1;

	SIM_assert(s_G3DrawCurVertIdx < G3_DRAW_MAX_VERTS);
	return;
}

static void G3SIM_ApplyDepthViewport()
{
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glProgramUniform4f(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "depthViewport"),
        viewport[0], viewport[1], viewport[2], viewport[3]);
}

void G3SIM_DrawArray()
{
	if( s_G3DrawCurVertIdx != 0 )
	{
		ApplyFieldEffect(sFieldEffectMode, sFieldEffectPhase, sFieldEffectBloom, sFieldSurface, sFieldContactShadow);
		G3SIM_ApplyDepthViewport();
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useWBuffer"), s_SIM_useWBuffer);
        GLint first = UploadDrawVertices(s_G3DrawVerts, s_G3DrawCurVertIdx);
        glDrawArrays(GL_TRIANGLES, first, s_G3DrawCurVertIdx);
	}
	return;
}

void G3SIM_DrawCleanUp()
{
	s_G3DrawCurVertIdx = 0;
}

void G3SIM_DrawInit()
{
    ++sDrawEpoch;
	glGenVertexArrays(1, &s_G3DrawVertexArray);
	glBindVertexArray(s_G3DrawVertexArray);

	glGenBuffers(1, &s_G3DrawVertexBuffer);

	glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(G3SIM_Vertex_t) * kStreamVertexCapacity, NULL, GL_STREAM_DRAW);
    sStreamVertexCursor = 0;


	glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, s));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, r));

	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, depthPlane));

	glBindVertexArray(0);
	glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(3);
}

static void G3SIM_ApplyFog(const g3_fog_state_t &fog)
{
    static GLuint program;
    static g3_fog_state_t applied;
    static unsigned epoch;
    static bool valid;
    if (valid && epoch == sDrawEpoch && program == g3shaderProgramID
        && ((!fog.enabled && !applied.enabled) || memcmp(&fog, &applied, sizeof(fog)) == 0)) {
        return;
    }
    program = g3shaderProgramID;
    applied = fog;
    epoch = sDrawEpoch;
    valid = true;
	// Set the fog attributes in the shader
	GLint useFogLoc = glGetUniformLocation(g3shaderProgramID, "useFog");
	int useFog = fog.enabled;
	glProgramUniform1i(g3shaderProgramID, useFogLoc , useFog);
	if(useFog) {
		// Fog color
		GLint fogColorLoc = glGetUniformLocation(g3shaderProgramID, "fogColor");
		u32 fogRed = fog.color & 0b11111;
		u32 fogGreen = (fog.color >> 5) & 0b11111;
		u32 fogBlue = (fog.color >> 10) & 0b11111;
		u32 fogAlpha = (fog.color >> 16) & 0b11111;
		float fogRedF = (float)fogRed / 32.0f;
		float fogGreenF = (float)fogGreen / 32.0f;
		float fogBlueF = (float)fogBlue / 32.0f;
		float fogAlphaF = (float)fogAlpha / 32.0f;
		glProgramUniform4f(g3shaderProgramID, fogColorLoc, fogRedF, fogGreenF, fogBlueF, fogAlphaF);


		u32 fogShift = (fog.control >> 8) & 0b1111;
		u32 fogStep = 0x400 >> fogShift;

		//Fog depth boundaries
		u32 fogOffset = fog.offset;

		float fogDepthBoundary[32];
		float fogDensity[32];
		const u8 * fogTablePtr = fog.density;
		for(int i=0; i < 32; i++) {
			fogDepthBoundary[i] = ((float)(fogOffset + fogStep*(i+1)) / 32767.0f) * 2.0f - 1.0f;
			fogDensity[i] = (float)fogTablePtr[i] / 127.0f;
		}

		GLint fogDepthBoundaryLoc = glGetUniformLocation(g3shaderProgramID, "fogDepthBoundary");
		glProgramUniform1fv(g3shaderProgramID, fogDepthBoundaryLoc, 32, fogDepthBoundary);

		GLint fogDensityLoc = glGetUniformLocation(g3shaderProgramID, "fogDensity");
		glProgramUniform1fv(g3shaderProgramID, fogDensityLoc, 32, fogDensity);
	}

}

static void G3SIM_ApplyTextureWrap(const G3SIM_TexImageParam_t &texture)
{
	// Flip applies only to repeated coordinates; otherwise the DS clamps to
	// the edge texel. Repeating clamped effect textures exposes extra copies.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		!texture.repeatS ? GL_CLAMP_TO_EDGE : (texture.flipS ? GL_MIRRORED_REPEAT : GL_REPEAT));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		!texture.repeatT ? GL_CLAMP_TO_EDGE : (texture.flipT ? GL_MIRRORED_REPEAT : GL_REPEAT));
}

static void G3SIM_ApplyPolygonRasterState(const G3SIM_PolygonAttr_t &polygon)
{
	glDepthFunc(polygon.depthTest ? GL_LEQUAL : GL_LESS);
	if(polygon.cullMode == GX_CULL_NONE) {
		glDisable(GL_CULL_FACE);
	} else {
		glEnable(GL_CULL_FACE);
		glCullFace(polygon.cullMode == GX_CULL_ALL ? GL_FRONT_AND_BACK :
			(polygon.cullMode == GX_CULL_FRONT ? GL_BACK : GL_FRONT));
	}
}

// COMP4x4 stores two-bit texels and a separate palette descriptor per block.
// Keep both streams and the referenced colors in the immutable cache identity.
static void G3SIM_DecodeCompressed(const u8 *source, const u8 *palette, u8 *out,
                                  u32 width, u32 height)
{
    const u32 blocks = width * height / 16;
    const u8 *indices = source + blocks * 4;
    for (u32 block = 0; block < blocks; ++block) {
        u16 descriptor;
        memcpy(&descriptor, indices + block * 2, 2);
        const u32 mode = descriptor >> 14;
        u16 colors[4];
        memcpy(colors, palette + block * 8, 8);
        if (mode & 1) {
            colors[2] = colors[3] = 0;
            for (u32 shift = 0; shift < 15; shift += 5) {
                const u32 a = (colors[0] >> shift) & 31;
                const u32 b = (colors[1] >> shift) & 31;
                colors[2] |= (mode == 1 ? (a + b) / 2 : (a * 5 + b * 3) / 8) << shift;
                if (mode == 3) colors[3] |= ((a * 3 + b * 5) / 8) << shift;
            }
        }
        const u32 x = (block % (width / 4)) * 4;
        const u32 y = (block / (width / 4)) * 4;
        for (u32 row = 0; row < 4; ++row) {
            for (u32 col = 0; col < 4; ++col) {
                const u32 index = (source[block * 4 + row] >> (col * 2)) & 3;
                u8 *pixel = out + ((y + row) * width + x + col) * 4;
                SIM_u16ToRGB(colors[index], pixel, pixel + 1, pixel + 2);
                pixel[3] = (mode < 2 && index == 3) ? 0 : 255;
            }
        }
    }
}

static G3Texture *G3SIM_AcquireTexture() {
	G3Texture *texture = nullptr;

	if (s_texImageParam.textureFormat != GX_TEXFMT_NONE) {
        u8 *vramTex;
#if defined(SDK_BUILD_LINUX) || defined(SDK_BUILD_WIN64)
        if (s_texImageParam.textureOffset >= 0x80000)
            vramTex = (u8 *)SIM_GetExtendedTextureStorage(s_texImageParam.textureOffset, 0);
        else
#endif
            vramTex = (u8 *)getTextureVramBank() + s_texImageParam.textureOffset;
        u8 *vramPltt = (u8 *)getTexPlttVramBank() + s_texPlttBase;
        const u32 pixels = s_texImageParam.textureSSize * s_texImageParam.textureTSize;
        u32 sourceSize = pixels, paletteSize = 0;
        std::vector<u8> compressedSource, compressedPalette;
        switch (s_texImageParam.textureFormat) {
        case GX_TEXFMT_PLTT4: sourceSize = pixels / 4; paletteSize = 8; break;
        case GX_TEXFMT_PLTT16: sourceSize = pixels / 2; paletteSize = 32; break;
        case GX_TEXFMT_PLTT256: paletteSize = 512; break;
        case GX_TEXFMT_A3I5: paletteSize = 64; break;
        case GX_TEXFMT_A5I3: paletteSize = 16; break;
        case GX_TEXFMT_DIRECT: sourceSize = pixels * 2; break;
        case GX_TEXFMT_COMP4x4: {
            const u32 blocks = pixels / 16;
            const u32 indexOffset = GX_COMP4x4_PLTT_IDX(s_texImageParam.textureOffset);
            const u8 *indices = (u8 *)getTextureVramBank() + indexOffset;
            compressedSource.assign(vramTex, vramTex + blocks * 4);
            compressedSource.insert(compressedSource.end(), indices, indices + blocks * 2);
            compressedPalette.resize(blocks * 8, 0);
            for (u32 block = 0; block < blocks; ++block) {
                u16 descriptor;
                memcpy(&descriptor, indices + block * 2, 2);
                const u32 mode = descriptor >> 14;
                const u32 colorBytes = (mode & 1) ? 4 : (mode == 0 ? 6 : 8);
                memcpy(compressedPalette.data() + block * 8,
                       vramPltt + (descriptor & 0x3fff) * 4, colorBytes);
            }
            vramTex = compressedSource.data();
            vramPltt = compressedPalette.data();
            sourceSize = compressedSource.size();
            paletteSize = compressedPalette.size();
            break;
        }
        default: SIM_assert_always(); return nullptr;
        }
        SIM_assert(vramTex != nullptr);
        // Content validation remains the correctness fallback for every direct,
        // physical and extended VRAM write. Metadata is part of the identity;
        // CRC matches are verified byte-for-byte, including palette changes.
        u64 key = ((u64)SIM_crc32buf(vramTex, sourceSize) << 32) |
                   (paletteSize ? SIM_crc32buf(vramPltt, paletteSize) : 0);
        auto matches = sGPUTextures.equal_range(key);
        for (auto i = matches.first; i != matches.second; ++i) {
            G3Texture *entry = i->second;
            if (entry->width == s_texImageParam.textureSSize && entry->height == s_texImageParam.textureTSize &&
                entry->format == s_texImageParam.textureFormat && entry->color0 == s_texImageParam.color0 &&
                entry->source.size() == sourceSize && entry->palette.size() == paletteSize &&
                !memcmp(entry->source.data(), vramTex, sourceSize) &&
                (!paletteSize || !memcmp(entry->palette.data(), vramPltt, paletteSize))) {
                texture = entry;
                SIM_ProfileAdd(SIM_PROFILE_TEXTURE_HITS, 1);
                break;
            }
        }
        if (!texture) {
            texture = new G3Texture;
            texture->width = s_texImageParam.textureSSize;
            texture->height = s_texImageParam.textureTSize;
            texture->format = s_texImageParam.textureFormat;
            texture->color0 = s_texImageParam.color0;
            texture->source.assign(vramTex, vramTex + sourceSize);
            texture->palette.assign(vramPltt, vramPltt + paletteSize);
            std::vector<u8> decoded(pixels * 4);
            u16 *colors = (u16 *)vramPltt;
            switch (texture->format) {
            case GX_TEXFMT_A3I5: G3SIM_DecodeTexA3I5(vramTex, colors, decoded.data(), texture->width, texture->height); break;
            case GX_TEXFMT_PLTT4: G3SIM_DecodeTex4(vramTex, colors, decoded.data(), texture->width, texture->height); break;
            case GX_TEXFMT_PLTT16: G3SIM_DecodeTex16(vramTex, colors, decoded.data(), texture->width, texture->height); break;
            case GX_TEXFMT_PLTT256: G3SIM_DecodeTex256(vramTex, colors, decoded.data(), texture->width, texture->height); break;
            case GX_TEXFMT_A5I3: G3SIM_DecodeTexA5I3(vramTex, colors, decoded.data(), texture->width, texture->height); break;
            case GX_TEXFMT_COMP4x4: G3SIM_DecodeCompressed(vramTex, vramPltt, decoded.data(), texture->width, texture->height); break;
            case GX_TEXFMT_DIRECT: G3SIM_DecodeTexDirect(vramTex, decoded.data(), texture->width, texture->height); break;
            }
            // Weighted strongly towards the brightest texels: lit glass, bulbs.
            double sum[3] = { }, weightSum = 0;
            for (u32 i = 0; i < pixels; i++) {
                const u8 *texel = &decoded[i * 4];
                if (!texel[3]) continue;
                double luma = (texel[0] + 2.0 * texel[1] + texel[2]) / (4 * 255.0), weight = luma * luma * luma * luma;
                for (int c = 0; c < 3; c++) sum[c] += texel[c] / 255.0 * weight;
                weightSum += weight;
            }
            if (weightSum > 0) {
                double peak = std::max(sum[0], std::max(sum[1], sum[2]));
                for (int c = 0; c < 3; c++) texture->glow[c] = (float)(sum[c] / peak);
            }
            glGenTextures(1, &texture->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture->id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded.data());
            sGPUBytes += pixels * 4;
            sUploadedBytes += pixels * 4;
            sSourceBytes += sourceSize + paletteSize;
            sGPUTextures.emplace(key, texture);
            SIM_ProfileAdd(SIM_PROFILE_UPLOADS, 1);
            SIM_ProfileAdd(SIM_PROFILE_UPLOAD_BYTES, pixels * 4);
        }
        texture->used = ++sTextureUse;
        G3SIM_TrimTextures(texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture->id);
        G3SIM_ApplyTextureWrap(s_texImageParam);

    }
    return texture;
}

u32 G3SIM_PrewarmTexture(u32 image, u32 palette) {
    const auto previous = s_texImageParam;
    const u32 previousPalette = s_texPlttBase;
    G3SIM_TexImageParam(image);
    G3SIM_TexPlttBase(palette);
    const u64 before = sUploadedBytes;
    G3Texture *entry = G3SIM_AcquireTexture();
    u32 bytes = sUploadedBytes - before;
    (void)entry;
    s_texImageParam = previous;
    s_texPlttBase = previousPalette;
    return bytes;
}

void G3SIM_FlushArray()
{
	if(s_G3DrawCurVertIdx == 0) {
		// No verts to draw
		return;
	}

	#ifdef SDK_TRACY_ENABLE
	TracyCZone(FlushArrayZone, 1);
	#endif

	const u64 textureProfile = SIM_ProfileNow();
	G3Texture *texture = G3SIM_AcquireTexture();
	ApplyTextureSmoothing(texture, sTextureSmoothing);
	CommitEmitters(texture);
    if (texture) {

		GLint texUnitLoc = glGetUniformLocation(g3shaderProgramID, "myTexture");
		//set texture 0 in the shader
		glProgramUniform1i(g3shaderProgramID, texUnitLoc , 0);

		//Enable texture in the shader
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 1);
	}
	else
	{
		//Disable texture in the shader
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 0);							
	}

	SIM_ProfileAdd(SIM_PROFILE_TEXTURE_NS, SIM_ProfileNow() - textureProfile);
	// Set the polygon mode in the shader
	GLint polygonModeLoc = glGetUniformLocation(g3shaderProgramID, "polygonMode");
	glProgramUniform1i(g3shaderProgramID, polygonModeLoc , s_curPolygonAttr.polygonMode);

	g3_fog_state_t fog = {};
	fog.control = s_reg_G3X_DISP3DCNT;
	fog.enabled = getbit(fog.control, 7) && s_curPolygonAttr.fogEnable;
	fog.color = s_reg_G3X_FOG_COLOR;
	fog.offset = s_reg_G3X_FOG_OFFSET;
	memcpy(fog.density, (const void *)&s_reg_G3X_FOG_TABLE_0, sizeof(fog.density));
	G3SIM_ApplyFog(fog);


	// If this is a translucent draw, save it for the end of the frame to render it last
        if (
            (s_texImageParam.textureFormat == GX_TEXFMT_A3I5
                || s_texImageParam.textureFormat == GX_TEXFMT_A5I3
                || s_curPolygonAttr.alpha < 0.99f || ((sFieldEffectMode == 8 || sFieldEffectMode == 22 || sFieldEffectMode == 23) && sFieldEffectPhase < 0.999f))
            && s_G3DrawCurVertIdx != 0) {
            // A3I5/A5I3 textures can contain opaque texels. Draw those with the
            // opaque scene; defer only their translucent texels for blending.
            // Tree atlases whose painted shadows fade (mode 22) split the same way.
            if (s_curPolygonAttr.alphaInt == 31 && !((sFieldEffectMode == 8 || sFieldEffectMode == 23) && sFieldEffectPhase < 0.999f)) {
                glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 1);
                glDepthMask(GL_TRUE);
                G3SIM_DrawArray();
            }
            glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 0);
            SIM_assert(s_G3DrawItemListCount < G3_DRAW_MAX_ITEMS);
            g3_draw_item_t *item = &s_G3DrawItemList[s_G3DrawItemListCount];

            memset(item, 0, sizeof(g3_draw_item_t));

            if (texture) {
                item->texture = texture;
                item->textureId = texture->id;
                texture->refs++;
            }

            item->fog = fog;
            item->smoothTexture = sTextureSmoothing;
            item->fieldEffectMode = sFieldEffectMode;
            item->fieldEffectPhase = sFieldEffectPhase;
            item->fieldEffectBloom = sFieldEffectBloom;
            memcpy(item->fieldContactShadow, sFieldContactShadow, sizeof(sFieldContactShadow));
            item->fieldSurface = sFieldSurface;
            item->useWBuffer = s_SIM_useWBuffer;

            // Copy over the polygonattr
            memcpy(&item->polygonAttr, &s_curPolygonAttr, sizeof(G3SIM_PolygonAttr_t));

            // Copy over the texImageParam
            memcpy(&item->texImageParam, &s_texImageParam, sizeof(G3SIM_TexImageParam_t));

            // Allocate and copy the verts
            item->verts = (G3SIM_Vertex_t *)(malloc(sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx));
            item->vertsCount = s_G3DrawCurVertIdx;
            memcpy(item->verts, s_G3DrawVerts, sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx);

            s_G3DrawItemListCount++;
        } else {
            glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 0);
            glDepthMask(GL_TRUE);
            G3SIM_DrawArray();
        }

        G3SIM_DrawCleanUp();
	#ifdef SDK_TRACY_ENABLE
	TracyCZoneEnd(FlushArrayZone);
	#endif
}

void G3SIM_DrawItems()
{
	//TODO sort the items by Z coordinate


	// The DS blends a translucent pixel only when its polygon ID differs from
	// the last translucent pixel at that location. Bit 6 distinguishes ID zero
	// from a pixel that has not been blended yet. Keep this across queue flushes;
	// the stencil buffer is reset with the 3D framebuffer each frame.
	glEnable(GL_STENCIL_TEST);
	glStencilMask(0x7f);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

	G3SIM_ApplyDepthViewport();

	//Draws all the translucent objects that were deferred to the end of the frame.
	for(int i=0; i<s_G3DrawItemListCount; i++) {
		g3_draw_item_t * item = &s_G3DrawItemList[i];
        SIM_ApplyPolygonStencil(item->fieldEffectMode, item->polygonAttr.polygonId);
		if(item->textureId != 0){
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, item->textureId);
            ApplyTextureSmoothing(item->texture, item->smoothTexture);
			//Enable texture in the shader
			glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 1);
		} else {
			glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 0);
		}

		if(item->textureId != 0) {
			G3SIM_ApplyTextureWrap(item->texImageParam);
		}

		// Set the polygon mode in the shader
		GLint polygonModeLoc = glGetUniformLocation(g3shaderProgramID, "polygonMode");
		glProgramUniform1i(g3shaderProgramID, polygonModeLoc, item->polygonAttr.polygonMode);

		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useWBuffer"), item->useWBuffer);
		G3SIM_ApplyFog(item->fog);
        ApplyFieldEffect(item->fieldEffectMode, item->fieldEffectPhase, item->fieldEffectBloom, item->fieldSurface, item->fieldContactShadow);
		G3SIM_ApplyPolygonRasterState(item->polygonAttr);
		glDepthMask(item->polygonAttr.translucentDepth ? GL_TRUE : GL_FALSE);
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 2);

        // Field-effect overlays (shadows, light pools, water, cones) keep the
        // fog of the surface underneath. Their own depth, raised or biased
        // towards the camera, would otherwise cut less foggy holes into it.
        bool keepFog = item->fieldEffectMode != 0;
        if (keepFog) glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        GLint first = UploadDrawVertices(item->verts, item->vertsCount);
                WaterTimer timer = { };
                bool timed = sWaterProfiling && item->fieldEffectMode == 3 && sWaterPending.size() < 64;
                if (timed) {
                    if (!sWaterFree.empty()) {
                        timer = sWaterFree.back();
                        sWaterFree.pop_back();
                    } else {
                        GLuint queries[2];
                        glGenQueries(2, queries);
                        timer.start = queries[0];
                        timer.end = queries[1];
                    }
                    timer.frame = sWaterFrame;
                    glQueryCounter(timer.start, GL_TIMESTAMP);
                }
                glDrawArrays(GL_TRIANGLES, first, item->vertsCount);
                if (timed) {
                    glQueryCounter(timer.end, GL_TIMESTAMP);
                    sWaterPending.push_back(timer);
                }
                if (keepFog) glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                free(item->verts);

		if(item->textureId != 0) {
			if (item->texture) item->texture->refs--;
            else glDeleteTextures(1, &item->textureId);
		}
	}
	s_G3DrawItemListCount = 0;
	G3SIM_TrimTextures(nullptr);
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xff);
	glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 0);
	glDepthMask(GL_TRUE);
	G3SIM_ApplyPolygonRasterState(s_curPolygonAttr);
}

void G3SIM_ClearTextureCache() {
    // Context thread only. Finish deferred versions before deleting resources.
    G3SIM_FlushArray();
    G3SIM_DrawItems();
    for (auto &item : sGPUTextures) {
        glDeleteTextures(1, &item.second->id);
        delete item.second;
    }
    sGPUTextures.clear();
    sGPUBytes = sSourceBytes = 0;
    SIM_ProfileSet(SIM_PROFILE_GPU_CACHE_BYTES, 0);
    SIM_ProfileSet(SIM_PROFILE_TEXTURE_SOURCE_BYTES, 0);
}
