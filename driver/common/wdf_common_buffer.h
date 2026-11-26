/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "dma_buffer_view.h"

/* Include after the KMDF headers. The caller supplies the device's DMA
 * enabler and an alignment mask (e.g. 15 for 16 bytes). On success the
 * caller owns the unpublished handle. On failure both outputs are unchanged. */
static inline NTSTATUS mt_dma_allocate_common_buffer32(WDFDMAENABLER enabler, size_t bytes,
                                                       ULONG alignmentMask, WDFCOMMONBUFFER *outHandle,
                                                       MT_DMA_BUFFER_VIEW *outView)
{
    if (!enabler || !outHandle || !outView ||
        !mt_dma_buffer_request_valid32(bytes, alignmentMask)) return STATUS_INVALID_PARAMETER;

    /* KMDF 1.11+ adds the mask to the raw length for above-page alignment.
     * The underlying DMA adapter takes that length as a ULONG. */
    const ULONG padding = alignmentMask >= PAGE_SIZE ? alignmentMask : 0u;
    if (bytes > UINT32_MAX - padding) return STATUS_INVALID_PARAMETER;

    WDF_COMMON_BUFFER_CONFIG config;
    WDF_COMMON_BUFFER_CONFIG_INIT(&config, alignmentMask);
    WDFCOMMONBUFFER handle;
    NTSTATUS status = WdfCommonBufferCreateWithConfig(enabler, bytes, &config,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &handle);
    if (!NT_SUCCESS(status)) return status;

    MT_DMA_BUFFER_VIEW view;
    view.cpu = (uint8_t *)WdfCommonBufferGetAlignedVirtualAddress(handle);
    view.logical = (uint64_t)WdfCommonBufferGetAlignedLogicalAddress(handle).QuadPart;
    view.bytes = WdfCommonBufferGetLength(handle);
    if (!mt_dma_buffer_view_valid32(&view, bytes, alignmentMask)) {
        WdfObjectDelete(handle);
        return STATUS_DATA_ERROR;
    }
    *outHandle = handle;
    *outView = view;
    return STATUS_SUCCESS;
}
