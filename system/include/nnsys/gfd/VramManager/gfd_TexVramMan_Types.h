#ifndef NNS_GFD_TEXVRAMMAN_TYPES_H_
#define NNS_GFD_TEXVRAMMAN_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <nitro.h>
#include <nnsys/gfd/gfd_common.h>

#define NNS_GFD_MASK_15BIT           0x7FFF
#define NNS_GFD_MASK_16BIT           0xFFFF

#ifdef LIBNTR_HAS_EXTENDED_TEXTURES
#define NNS_GFD_TEXKEY_SIZE_SHIFT    7
#define NNS_GFD_TEXKEY_SIZE_MASK     0xFFF
#define NNS_GFD_TEXKEY_ADDR_MASK     0x7FFFF
#define NNS_GFD_TEXKEY_SIZE_POS      19
#else
#define NNS_GFD_TEXKEY_SIZE_SHIFT    4
#define NNS_GFD_TEXKEY_SIZE_MASK     0x7FFF
#define NNS_GFD_TEXKEY_ADDR_MASK     0xFFFF
#define NNS_GFD_TEXKEY_SIZE_POS      16
#endif
#define NNS_GFD_TEXKEY_ADDR_SHIFT    3

#define NNS_GFD_TEXSIZE_MIN          (0x1 << NNS_GFD_TEXKEY_SIZE_SHIFT)
#define NNS_GFD_TEXSIZE_MAX          (NNS_GFD_TEXKEY_SIZE_MASK << NNS_GFD_TEXKEY_SIZE_SHIFT)

#define NNS_GFD_ALLOC_ERROR_TEXKEY   (u32)0x0

typedef u32 NNSGfdTexKey;

typedef NNSGfdTexKey (*NNSGfdFuncAllocTexVram)(u32 szByte, BOOL is4x4comp, u32 opt);
typedef int (*NNSGfdFuncFreeTexVram)(NNSGfdTexKey key);

extern NNSGfdFuncAllocTexVram NNS_GfdDefaultFuncAllocTexVram;
extern NNSGfdFuncFreeTexVram NNS_GfdDefaultFuncFreeTexVram;

#ifdef SDK_PORT
static
#endif
NNS_GFD_INLINE NNSGfdTexKey NNS_GfdAllocTexVram (u32 szByte, BOOL is4x4comp, u32 opt)
{
    return (*NNS_GfdDefaultFuncAllocTexVram)(szByte, is4x4comp, opt);
}

#ifdef SDK_PORT
static
#endif
NNS_GFD_INLINE int NNS_GfdFreeTexVram (NNSGfdTexKey memKey)
{
    return (*NNS_GfdDefaultFuncFreeTexVram)(memKey);
}

#ifdef SDK_PORT
static
#endif
NNS_GFD_INLINE u32 NNSi_GfdGetTexKeyRoundupSize (u32 size)
{
    if (size == 0) {
        return NNS_GFD_TEXSIZE_MIN;
    } else {

        return (((u32)(size) + (NNS_GFD_TEXSIZE_MIN - 1)) & ~(NNS_GFD_TEXSIZE_MIN - 1));
    }
}

#ifdef SDK_PORT
static
#endif
NNS_GFD_INLINE NNSGfdTexKey NNS_GfdMakeTexKey (u32 addr, u32 size, BOOL b4x4Comp)
{
    SDK_ASSERT((addr & (u32)((0x1 << NNS_GFD_TEXKEY_ADDR_SHIFT) - 1)) == 0);
    SDK_ASSERT((size & (u32)((0x1 << NNS_GFD_TEXKEY_SIZE_SHIFT) - 1)) == 0);

    SDK_ASSERT(((size >> NNS_GFD_TEXKEY_SIZE_SHIFT) & ~NNS_GFD_TEXKEY_SIZE_MASK) == 0);
    SDK_ASSERT(((addr >> NNS_GFD_TEXKEY_ADDR_SHIFT) & ~NNS_GFD_TEXKEY_ADDR_MASK) == 0);

    return ((size >> NNS_GFD_TEXKEY_SIZE_SHIFT) << NNS_GFD_TEXKEY_SIZE_POS)
           | ((NNS_GFD_TEXKEY_ADDR_MASK & (addr >> NNS_GFD_TEXKEY_ADDR_SHIFT)))
           | b4x4Comp << 31;
}

#ifdef SDK_PORT
static
#endif
NNS_GFD_INLINE u32 NNS_GfdGetTexKeyAddr (NNSGfdTexKey memKey)
{
    return (u32)(((NNS_GFD_TEXKEY_ADDR_MASK & memKey)) << NNS_GFD_TEXKEY_ADDR_SHIFT);
}

#ifdef SDK_PORT
static
#endif
NNS_GFD_INLINE u32 NNS_GfdGetTexKeySize (NNSGfdTexKey memKey)
{
    return (u32)(((memKey >> NNS_GFD_TEXKEY_SIZE_POS) & NNS_GFD_TEXKEY_SIZE_MASK) << NNS_GFD_TEXKEY_SIZE_SHIFT);
}

#ifdef SDK_PORT
static
#endif
NNS_GFD_INLINE BOOL NNS_GfdGetTexKey4x4Flag (NNSGfdTexKey memKey)
{
    return (BOOL)((0x80000000 & memKey) >> 31);
}

#ifdef __cplusplus
}
#endif

#endif
