#ifdef SDK_ARM9
#include <nitro.h>

#include "../include/mi_dma.h"

void MI_HBlankDmaCopy32 (u32 dmaNo, const void * src, void * dest, u32 size)
{
	MIi_ASSERT_DMANO(dmaNo);
	MIi_ASSERT_SRC_ALIGN4(src);
	MIi_ASSERT_DEST_ALIGN4(dest);
	MIi_WARNING_ADDRINTCM(src, size);
	MIi_WARNING_ADDRINTCM(dest, size);

	MIi_CheckAnotherAutoDMA(dmaNo, MI_DMA_TIMING_H_BLANK);
	MIi_CheckDma0SourceAddress(dmaNo, (u32)src, size, MI_DMA_SRC_INC);

	if (size == 0) {
		return;
	}

	MI_WaitDma(dmaNo);
	MIi_DmaSetParams(dmaNo, (u32)src, (u32)dest, MI_CNT_HBCOPY32(size));
}

void MI_HBlankDmaCopy16 (u32 dmaNo, const void * src, void * dest, u32 size)
{
	MIi_ASSERT_DMANO(dmaNo);
	MIi_ASSERT_SRC_ALIGN2(src);
	MIi_ASSERT_DEST_ALIGN2(dest);
	MIi_WARNING_ADDRINTCM(src, size);
	MIi_WARNING_ADDRINTCM(dest, size);

	MIi_CheckAnotherAutoDMA(dmaNo, MI_DMA_TIMING_H_BLANK);
	MIi_CheckDma0SourceAddress(dmaNo, (u32)src, size, MI_DMA_SRC_INC);

	if (size == 0) {
		return;
	}

	MI_WaitDma(dmaNo);
	MIi_DmaSetParams(dmaNo, (u32)src, (u32)dest, MI_CNT_HBCOPY16(size));
}

void MI_HBlankDmaCopy32If (u32 dmaNo, const void * src, void * dest, u32 size)
{
	MIi_ASSERT_DMANO(dmaNo);
	MIi_ASSERT_SRC_ALIGN4(src);
	MIi_ASSERT_DEST_ALIGN4(dest);
	MIi_WARNING_ADDRINTCM(src, size);
	MIi_WARNING_ADDRINTCM(dest, size);

	MIi_CheckAnotherAutoDMA(dmaNo, MI_DMA_TIMING_H_BLANK);
	MIi_CheckDma0SourceAddress(dmaNo, (u32)src, size, MI_DMA_SRC_INC);

	if (size == 0) {
		return;
	}

	MI_WaitDma(dmaNo);
	MIi_DmaSetParams(dmaNo, (u32)src, (u32)dest, MI_CNT_HBCOPY32_IF(size));
}

void MI_HBlankDmaCopy16If (u32 dmaNo, const void * src, void * dest, u32 size)
{
	MIi_ASSERT_DMANO(dmaNo);
	MIi_ASSERT_SRC_ALIGN2(src);
	MIi_ASSERT_DEST_ALIGN2(dest);
	MIi_WARNING_ADDRINTCM(src, size);
	MIi_WARNING_ADDRINTCM(dest, size);

	MIi_CheckAnotherAutoDMA(dmaNo, MI_DMA_TIMING_H_BLANK);
	MIi_CheckDma0SourceAddress(dmaNo, (u32)src, size, MI_DMA_SRC_INC);

	if (size == 0) {
		return;
	}

	MI_WaitDma(dmaNo);
	MIi_DmaSetParams(dmaNo, (u32)src, (u32)dest, MI_CNT_HBCOPY16_IF(size));
}
#elif defined(SDK_PORT)
#include <nitro.h>
#include "../include/mi_dma.h"
// Each visible HBlank transfers one entry and reloads the destination.
// Keep host pointers intact: the emulated DMA address registers are only 32-bit.
static struct {
    const u8 *source;
    void *destination;
    u32 size;
} s_HBlankDma[MI_DMA_MAX_NUM + 1];

static void MIi_StartHBlankDma(u32 dmaNo, const void *src, void *dest, u32 size)
{
    MIi_ASSERT_DMANO(dmaNo);
    if (size == 0) {
        return;
    }
    s_HBlankDma[dmaNo].source = src;
    s_HBlankDma[dmaNo].destination = dest;
    s_HBlankDma[dmaNo].size = size;
}

void MIi_StopHBlankDma(u32 dmaNo)
{
    MIi_ASSERT_DMANO(dmaNo);
    s_HBlankDma[dmaNo].size = 0;
}

void MIi_ExecuteHBlankDma(void)
{
    for (u32 dmaNo = 0; dmaNo <= MI_DMA_MAX_NUM; dmaNo++) {
        if (s_HBlankDma[dmaNo].size) {
            memcpy(s_HBlankDma[dmaNo].destination, s_HBlankDma[dmaNo].source,
                   s_HBlankDma[dmaNo].size);
            s_HBlankDma[dmaNo].source += s_HBlankDma[dmaNo].size;
        }
    }
}

BOOL MIi_HBlankDmaWritesTo(const volatile void *address)
{
    for (u32 dmaNo = 0; dmaNo <= MI_DMA_MAX_NUM; dmaNo++) {
        const u64 start = (u64)s_HBlankDma[dmaNo].destination;
        if (s_HBlankDma[dmaNo].size && (u64)address >= start &&
            (u64)address < start + s_HBlankDma[dmaNo].size) {
            return TRUE;
        }
    }
    return FALSE;
}

void MI_HBlankDmaCopy32(u32 dmaNo, const void *src, void *dest, u32 size)
{
    MIi_StartHBlankDma(dmaNo, src, dest, size);
}

void MI_HBlankDmaCopy16(u32 dmaNo, const void *src, void *dest, u32 size)
{
    MIi_StartHBlankDma(dmaNo, src, dest, size);
}

void MI_HBlankDmaCopy32If(u32 dmaNo, const void *src, void *dest, u32 size)
{
    MIi_StartHBlankDma(dmaNo, src, dest, size);
}

void MI_HBlankDmaCopy16If(u32 dmaNo, const void *src, void *dest, u32 size)
{
    MIi_StartHBlankDma(dmaNo, src, dest, size);
}
#endif
