#ifndef SIM_FRAME_PACER_H
#define SIM_FRAME_PACER_H
#include <cstdint>
// Absolute monotonic deadlines retain small overruns without accumulating drift.
// Long stalls discard debt so a resumed game does not run a burst of updates.
struct SIM_FramePacer {
    uint64_t deadline = 0, interval = 0;
    int mode = -1;
    uint64_t target(uint64_t frameStart, uint64_t period, int policy) {
        if (!deadline || interval != period || mode != policy) {
            interval = period; mode = policy; deadline = frameStart + period;
        }
        return deadline;
    }
    void finish(uint64_t now) {
        deadline += interval;
        if (now > deadline && now - deadline > interval) deadline = now + interval;
    }
    void reset() { deadline = interval = 0; mode = -1; }
};
#endif
