/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *cpu;
    uint64_t logical;
    size_t bytes;
} MT_DMA_BUFFER_VIEW;

/* WDF uses alignment in bytes minus one. Keep the byte alignment
 * representable in 32 bits; a mask of zero means byte alignment. */
static inline int mt_dma_buffer_request_valid32(size_t bytes, uint32_t alignmentMask)
{
    return bytes != 0 && bytes - 1 <= UINT32_MAX &&
           alignmentMask != UINT32_MAX && (alignmentMask & (alignmentMask + 1u)) == 0;
}

static inline int mt_dma_buffer_view_valid32(const MT_DMA_BUFFER_VIEW *view,
                                              size_t bytes, uint32_t alignmentMask)
{
    if (!view || !view->cpu || view->bytes != bytes ||
        !mt_dma_buffer_request_valid32(bytes, alignmentMask)) return 0;

    const uintptr_t cpu = (uintptr_t)view->cpu;
    const size_t lastOffset = bytes - 1;
    return ((cpu | view->logical) & alignmentMask) == 0 &&
           lastOffset <= UINTPTR_MAX - cpu && view->logical <= UINT32_MAX &&
           lastOffset <= UINT32_MAX - view->logical;
}
