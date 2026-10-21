#include <nitro/gx/g2.h>
#include <nitro/fx/fx_const.h>

#include <nitro/code32.h>


#ifdef SDK_PORT
/* Simulator registers are independent globals, not a memory-mapped block. */
static vu16 *G2i_BlendRegister(u64 addr, int index)
{
    if (addr == (u64)&reg_G2_BLDCNT || addr == 0x04000050) {
        vu16 *regs[] = { &reg_G2_BLDCNT, &reg_G2_BLDALPHA, &reg_G2_BLDY };
        return regs[index];
    }
    if (addr == (u64)&reg_G2S_DB_BLDCNT || addr == 0x04001050) {
        vu16 *regs[] = { &reg_G2S_DB_BLDCNT, &reg_G2S_DB_BLDALPHA, &reg_G2S_DB_BLDY };
        return regs[index];
    }
    return (vu16 *)addr + index;
}
typedef struct {
    vu16 *pa, *pb, *pc, *pd;
    vu32 *x, *y;
} G2iAffineRegisters;

static G2iAffineRegisters G2i_GetAffineRegisters(u64 addr)
{
    const G2iAffineRegisters regs[] = {
        { &reg_G2_BG2PA, &reg_G2_BG2PB, &reg_G2_BG2PC, &reg_G2_BG2PD, &reg_G2_BG2X, &reg_G2_BG2Y },
        { &reg_G2_BG3PA, &reg_G2_BG3PB, &reg_G2_BG3PC, &reg_G2_BG3PD, &reg_G2_BG3X, &reg_G2_BG3Y },
        { &reg_G2S_DB_BG2PA, &reg_G2S_DB_BG2PB, &reg_G2S_DB_BG2PC, &reg_G2S_DB_BG2PD, &reg_G2S_DB_BG2X, &reg_G2S_DB_BG2Y },
        { &reg_G2S_DB_BG3PA, &reg_G2S_DB_BG3PB, &reg_G2S_DB_BG3PC, &reg_G2S_DB_BG3PD, &reg_G2S_DB_BG3X, &reg_G2S_DB_BG3Y }
    };
    const u64 dsAddresses[] = { 0x04000020, 0x04000030, 0x04001020, 0x04001030 };
    for (int i = 0; i < 4; i++) {
        if (addr == (u64)regs[i].pa || addr == dsAddresses[i]) return regs[i];
    }
    G2iAffineRegisters local = { (vu16 *)addr, (vu16 *)addr + 1,
        (vu16 *)addr + 2, (vu16 *)addr + 3, (vu32 *)addr + 2, (vu32 *)addr + 3 };
    return local;
}
#else
#define G2i_BlendRegister(addr, index) ((vu16 *)(addr) + (index))
#endif

#ifdef SDK_PORT
void G2x_SetBGyAffine_ (u64 addr, const MtxFx22 *mtx, int centerX, int centerY, int x1, int y1)
#else
void G2x_SetBGyAffine_ (u32 addr, const MtxFx22 *mtx, int centerX, int centerY, int x1, int y1)
#endif
{
    s32 dx, dy;
    fx32 x2, y2;

    SDK_NULL_ASSERT(mtx);
    SDK_MINMAX_ASSERT(mtx->_00, -128 * FX32_ONE, 128 * FX32_ONE - 1);
    SDK_MINMAX_ASSERT(mtx->_01, -128 * FX32_ONE, 128 * FX32_ONE - 1);
    SDK_MINMAX_ASSERT(mtx->_10, -128 * FX32_ONE, 128 * FX32_ONE - 1);
    SDK_MINMAX_ASSERT(mtx->_11, -128 * FX32_ONE, 128 * FX32_ONE - 1);

#ifdef SDK_PORT
    G2iAffineRegisters regs = G2i_GetAffineRegisters(addr);
    *regs.pa = (u16)(s16)(mtx->_00 >> 4);
    *regs.pb = (u16)(s16)(mtx->_01 >> 4);
    *regs.pc = (u16)(s16)(mtx->_10 >> 4);
    *regs.pd = (u16)(s16)(mtx->_11 >> 4);
#else
    *((vu32 *)addr + 0) = (u32)((u16)(s16)(mtx->_00 >> 4) | (u16)(s16)(mtx->_01 >> 4) << 16);
    *((vu32 *)addr + 1) = (u32)((u16)(s16)(mtx->_10 >> 4) | (u16)(s16)(mtx->_11 >> 4) << 16);
#endif

    dx = x1 - centerX;
    dy = y1 - centerY;

    x2 = mtx->_00 * dx + mtx->_01 * dy + (centerX << FX32_SHIFT);
    y2 = mtx->_10 * dx + mtx->_11 * dy + (centerY << FX32_SHIFT);

#ifdef SDK_PORT
    *regs.x = (u32)(x2 >> 4);
    *regs.y = (u32)(y2 >> 4);
#else
    *((vu32 *)addr + 2) = (u32)(x2 >> 4);
    *((vu32 *)addr + 3) = (u32)(y2 >> 4);
#endif
}

#include <nitro/codereset.h>

typedef enum {
    G2_BLENDTYPE_NONE    = 0x0000,
    G2_BLENDTYPE_ALPHA   = 0x0040,
    G2_BLENDTYPE_FADEIN  = 0x0080,
    G2_BLENDTYPE_FADEOUT = 0x00c0
} G2_BLENDTYPE;

#ifdef SDK_PORT
void G2x_SetBlendAlpha_ (u64 addr, int plane1, int plane2, int ev1, int ev2)
#else
void G2x_SetBlendAlpha_ (u32 addr, int plane1, int plane2, int ev1, int ev2)
#endif
{
    SDK_MINMAX_ASSERT(plane1, 0, 0x20 - 1);
    SDK_MINMAX_ASSERT(plane2, 0, 0x40 - 1);
    SDK_MINMAX_ASSERT(ev1, 0, 31);
    SDK_MINMAX_ASSERT(ev2, 0, 31);

#ifdef SDK_PORT
    *G2i_BlendRegister(addr, 0) = (u16)(G2_BLENDTYPE_ALPHA | plane1 | plane2 << 8);
    *G2i_BlendRegister(addr, 1) = (u16)(ev1 | ev2 << 8);
#else
    *((vu32 *)addr + 0) = (u32)((G2_BLENDTYPE_ALPHA | plane1 | plane2 << 8) | ((ev1 | ev2 << 8) << 16));
#endif
}

#ifdef SDK_PORT
void G2x_SetBlendBrightness_ (u64 addr, int plane, int brightness)
#else
void G2x_SetBlendBrightness_ (u32 addr, int plane, int brightness)
#endif
{
    SDK_MINMAX_ASSERT(brightness, -31, 31);
    SDK_MINMAX_ASSERT(plane, 0, 0x40 - 1);

    if (brightness < 0) {
        *G2i_BlendRegister(addr, 0) = (u16)(G2_BLENDTYPE_FADEOUT | plane);
        *G2i_BlendRegister(addr, 2) = (u16) - brightness;
    } else {
        *G2i_BlendRegister(addr, 0) = (u16)(G2_BLENDTYPE_FADEIN | plane);
        *G2i_BlendRegister(addr, 2) = (u16)brightness;
    }
}

#ifdef SDK_PORT
void G2x_SetBlendBrightnessExt_ (u64 addr, int plane1, int plane2, int ev1, int ev2, int brightness)
#else
void G2x_SetBlendBrightnessExt_ (u32 addr, int plane1, int plane2, int ev1, int ev2, int brightness)
#endif
{
    SDK_MINMAX_ASSERT(brightness, -31, 31);
    SDK_MINMAX_ASSERT(plane1, 0, 0x40 - 1);
    SDK_MINMAX_ASSERT(plane2, 0, 0x40 - 1);
    SDK_MINMAX_ASSERT(ev1, 0, 31);
    SDK_MINMAX_ASSERT(ev2, 0, 31);

    *G2i_BlendRegister(addr, 1) = (u16)(ev1 | (ev2 << 8));

    if (brightness < 0) {
        *G2i_BlendRegister(addr, 0) = (u16)(G2_BLENDTYPE_FADEOUT | plane1 | (plane2 << 8));
        *G2i_BlendRegister(addr, 2) = (u16) - brightness;
    } else {
        *G2i_BlendRegister(addr, 0) = (u16)(G2_BLENDTYPE_FADEIN | plane1 | (plane2 << 8));
        *G2i_BlendRegister(addr, 2) = (u16)brightness;
    }
}

#ifdef SDK_PORT
void G2x_ChangeBlendBrightness_ (u64 addr, int brightness)
#else
void G2x_ChangeBlendBrightness_ (u32 addr, int brightness)
#endif
{
    u16 tmp;
    SDK_MINMAX_ASSERT(brightness, -31, 31);

    tmp = *G2i_BlendRegister(addr, 0);

    if (brightness < 0) {
        if (G2_BLENDTYPE_FADEIN == (tmp & REG_G2_BLDCNT_EFFECT_MASK)) {
            *G2i_BlendRegister(addr, 0) = (u16)((tmp & ~REG_G2_BLDCNT_EFFECT_MASK) | G2_BLENDTYPE_FADEOUT);
        }

        *G2i_BlendRegister(addr, 2) = (u16) - brightness;
    } else {
        if (G2_BLENDTYPE_FADEOUT == (tmp & REG_G2_BLDCNT_EFFECT_MASK)) {
            *G2i_BlendRegister(addr, 0) = (u16)((tmp & ~REG_G2_BLDCNT_EFFECT_MASK) | G2_BLENDTYPE_FADEIN);
        }

        *G2i_BlendRegister(addr, 2) = (u16)brightness;
    }
}
