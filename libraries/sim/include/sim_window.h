#ifndef SIM_WINDOW_H
#define SIM_WINDOW_H

#include <stdint.h>

struct SIM_WindowState {
  uint32_t display;
  uint16_t horizontal[2];
  uint16_t vertical[2];
  uint16_t inside;
  uint16_t outside;
};

// DS window intervals exclude the end coordinate and wrap at 256.
static inline bool SIM_WindowContains(int coordinate, uint16_t bounds) {
  const int start = bounds >> 8;
  const int end = bounds & 255;
  if (coordinate < 0 || coordinate >= 256) return false;
  return start <= end ? coordinate >= start && coordinate < end
                      : coordinate >= start || coordinate < end;
}

static inline uint8_t SIM_WindowMask(const SIM_WindowState &state, int x, int y) {
  if (!(state.display & 0xe000)) return 0x3f;
  // WIN0 takes precedence over WIN1, then the OBJ window, then WINOUT.
  for (int window = 0; window < 2; window++) {
    if ((state.display & (0x2000 << window)) &&
        SIM_WindowContains(x, state.horizontal[window]) &&
        SIM_WindowContains(y, state.vertical[window]))
      return (state.inside >> (window * 8)) & 0x3f;
  }
  // Bit 7 tells the compositor that an OBJ window may override WINOUT.
  return (state.outside & 0x3f) | ((state.display & 0x8000) ? 0x80 : 0);
}

#endif
