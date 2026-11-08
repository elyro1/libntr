#ifndef SIM_G2_H
#define SIM_G2_H
#include <nitro/types.h>
// On an OBJ pixel of G2SIM_DrawOBJ: its color index is in the OBJ extension's palettes (SIM_SetObjExtension)
#define OBJ_PIXEL_EXT_PLTT 0x200000
void G2SIM_DrawBG( u8 bgNum, u8 bgMode, u8 bg03D, u8 isSub );
void G2SIM_DrawTrainerBannerWide(void);
BOOL G2SIM_HasFieldSprites(void);
BOOL G2SIM_HasFieldSpriteEdges(void);
BOOL G2SIM_IsFieldMoveCutInWide(void);
extern u32 s_FieldSpriteLine[];
void G2SIM_DrawOBJ( u32 line, u8 isSub, int bgnum, BOOL fieldOnly );
void G2SIM_PlotPixel(u8 * screenBuf, u8 r, u8 g, u8 b, u32 x, u32 y, BOOL isSub);
void G2SIM_CaptureScanline(u32 line);
const u8 *G2SIM_GetWindowMasks(BOOL isSub);
const u16 *G2SIM_GetScanlineBlend(BOOL isSub);
// Whether BG2 or BG3 (bg 2 or 3) kept one matrix over the frame's lines, its reference advancing by PB and PD each
// line with no HBlank change; then its PA to PD and line 0's reference, in the registers' units
BOOL G2SIM_GetFrameAffine(BOOL isSub, int bg, s16 matrix[4], s32 *x, s32 *y);
#endif
