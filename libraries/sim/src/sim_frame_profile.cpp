#include <simulator/sim_frame_profile.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

static FILE *sTrace;
static std::atomic<u64> sCounters[SIM_PROFILE_COUNT];
static std::atomic<int> sContext[5];
static u64 sFrame;
extern "C" void SIM_ProfileInit(void) {
    const char *path = std::getenv("PLATINUM_FRAME_TRACE");
    if (!path || !*path) return;
    sTrace = std::fopen(path, "w");
    if (!sTrace) { std::perror("PLATINUM_FRAME_TRACE"); return; }
    // Buffer output; tracing must not flush to disk at each frame boundary.
    std::setvbuf(sTrace, nullptr, _IOFBF, 1024 * 1024);
    std::fprintf(sTrace, "frame,work_ns,wait_ns,swap_ns,full_ns,vsync,speed,x,z,header,matrix,movement,field_ns,chunk_ns,props_ns,collision_ns,zone_ns,texture_ns,uploads,upload_bytes,texture_hits,narc_disk_bytes,narc_ram_bytes,prepare_ns,ready_hits,ready_misses,prepare_fallbacks,cpu_cache_bytes,gpu_cache_bytes,texture_source_bytes,gpu_ns,gpu_result_frame\n");
    std::atexit([] { if (sTrace) std::fclose(sTrace); });
}
extern "C" BOOL SIM_ProfileEnabled(void) { return sTrace != nullptr; }
extern "C" u64 SIM_ProfileNow(void) {
    if (!sTrace) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void SIM_ProfileAdd(int counter, u64 value) {
    if (sTrace) sCounters[counter].fetch_add(value, std::memory_order_relaxed);
}
extern "C" void SIM_ProfileSet(int counter, u64 value) {
    if (sTrace) sCounters[counter].store(value, std::memory_order_relaxed);
}
extern "C" void SIM_ProfileContext(int x, int z, int header, int matrix, int speed) {
    if (!sTrace) return;
    const int values[] = {x,z,header,matrix,speed};
    for (int i=0;i<5;i++) sContext[i].store(values[i], std::memory_order_relaxed);
}
extern "C" void SIM_ProfileFrame(u64 work, u64 wait, u64 swap, u64 full, int vsync, float speed) {
    if (!sTrace) return;
    std::fprintf(sTrace, "%llu,%llu,%llu,%llu,%llu,%d,%.3f", (unsigned long long)sFrame++, (unsigned long long)work, (unsigned long long)wait, (unsigned long long)swap, (unsigned long long)full, vsync, speed);
    for (auto &value : sContext) std::fprintf(sTrace, ",%d", value.load(std::memory_order_relaxed));
    for (int i=0;i<SIM_PROFILE_COUNT;i++) {
        u64 value = i >= SIM_PROFILE_CPU_CACHE_BYTES ? sCounters[i].load(std::memory_order_relaxed) : sCounters[i].exchange(0, std::memory_order_relaxed);
        std::fprintf(sTrace, ",%llu", (unsigned long long)value);
    }
    std::fputc('\n', sTrace);
}
