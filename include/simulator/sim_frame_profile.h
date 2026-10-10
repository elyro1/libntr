#ifndef SIM_FRAME_PROFILE_H
#define SIM_FRAME_PROFILE_H
#include <nitro/types.h>
#ifdef __cplusplus
extern "C" {
#endif
enum SIM_ProfileCounter {
    SIM_PROFILE_FIELD_NS, SIM_PROFILE_CHUNK_NS, SIM_PROFILE_PROPS_NS,
    SIM_PROFILE_COLLISION_NS, SIM_PROFILE_ZONE_NS, SIM_PROFILE_TEXTURE_NS,
    SIM_PROFILE_UPLOADS, SIM_PROFILE_UPLOAD_BYTES, SIM_PROFILE_TEXTURE_HITS,
    SIM_PROFILE_NARC_DISK_BYTES, SIM_PROFILE_NARC_RAM_BYTES,
    SIM_PROFILE_PREPARE_NS, SIM_PROFILE_READY_HITS, SIM_PROFILE_READY_MISSES,
    SIM_PROFILE_PREPARE_FALLBACKS,
    SIM_PROFILE_CPU_CACHE_BYTES, SIM_PROFILE_GPU_CACHE_BYTES,
    SIM_PROFILE_TEXTURE_SOURCE_BYTES, SIM_PROFILE_GPU_NS, SIM_PROFILE_GPU_FRAME,
    SIM_PROFILE_COUNT
};
void SIM_ProfileInit(void);
BOOL SIM_ProfileEnabled(void);
u64 SIM_ProfileNow(void);
void SIM_ProfileAdd(int counter, u64 value);
void SIM_ProfileSet(int counter, u64 value);
void SIM_ProfileContext(int x, int z, int header, int matrix, int speed);
void SIM_ProfileFrame(u64 work, u64 wait, u64 swap, u64 full, int vsync, float speed);
#ifdef __cplusplus
}
#endif
#endif
