#ifndef SIM_G2_H
#define SIM_G2_H
#include <nitro/types.h>
// On an OBJ pixel of G2SIM_DrawOBJ: its color index is in the OBJ extension's palettes (SIM_SetObjExtension)
#define OBJ_PIXEL_EXT_PLTT 0x200000
void G2SIM_DrawBG( u8 bgNum, u8 bgMode, u8 bg03D, u8 isSub );
void G2SIM_DrawTrainerBannerWide(void);
// SIM_SetWideUI: draws the 256 dots after the native screen in a main-engine
// text BG's 512-dot wrap into half (256 wide, both screens like the native
// buffers). FALSE when the layer is not a text BG and nothing was drawn.
BOOL G2SIM_DrawBGWideHalf(u8 bgNum, u8 bgMode, u8 bg03D, u8 *half);
// SIM_SetWideUI: field-only OBJ drawing takes every main-engine sprite.
void G2SIM_SetWideObjAll(BOOL enabled);
BOOL G2SIM_HasFieldSprites(void);
BOOL G2SIM_HasFieldSpriteEdges(void);
BOOL G2SIM_IsFieldMoveCutInWide(void);
extern u32 s_FieldSpriteLine[];
void G2SIM_DrawOBJ( u32 line, u8 isSub, int bgnum, BOOL fieldOnly );
void G2SIM_PlotPixel(u8 * screenBuf, u8 r, u8 g, u8 b, u32 x, u32 y, BOOL isSub);
void G2SIM_CaptureScanline(u32 line);
const u8 *G2SIM_GetWindowMasks(BOOL isSub);
const u16 *G2SIM_GetScanlineBlend(BOOL isSub);
#endif
