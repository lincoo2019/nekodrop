/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(EXIT_FAILURE); \
} } while (0)

/* The synthetic VA and logical address are metadata. No test dereferences either. */
typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef struct { int tag; } FAKE_ENABLER, *WDFDMAENABLER;
typedef struct { int64_t QuadPart; } PHYSICAL_ADDRESS;
typedef struct {
    void *cpu;
    PHYSICAL_ADDRESS logical;
    size_t bytes;
} FAKE_BUFFER, *WDFCOMMONBUFFER;
typedef struct { ULONG mask; } WDF_COMMON_BUFFER_CONFIG;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000du)
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003eu)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009au)
#define NT_SUCCESS(status) ((status) >= 0)
#define WDF_NO_OBJECT_ATTRIBUTES NULL
#define PAGE_SIZE 4096u

static struct {
    FAKE_BUFFER buffer;
    NTSTATUS result;
    WDFDMAENABLER enabler;
    size_t requestedBytes;
    ULONG mask;
    unsigned creates;
    unsigned deletes;
} fake;

static void WDF_COMMON_BUFFER_CONFIG_INIT(WDF_COMMON_BUFFER_CONFIG *config, ULONG mask)
{
    config->mask = mask;
}

static NTSTATUS WdfCommonBufferCreateWithConfig(WDFDMAENABLER enabler, size_t bytes,
    const WDF_COMMON_BUFFER_CONFIG *config, void *attributes, WDFCOMMONBUFFER *handle)
{
    CHECK(attributes == WDF_NO_OBJECT_ATTRIBUTES);
    fake.enabler = enabler;
    fake.requestedBytes = bytes;
    fake.mask = config->mask;
    ++fake.creates;
    if (NT_SUCCESS(fake.result)) *handle = &fake.buffer;
    return fake.result;
}

static size_t WdfCommonBufferGetLength(WDFCOMMONBUFFER handle) { return handle->bytes; }
static void *WdfCommonBufferGetAlignedVirtualAddress(WDFCOMMONBUFFER handle) { return handle->cpu; }
static PHYSICAL_ADDRESS WdfCommonBufferGetAlignedLogicalAddress(WDFCOMMONBUFFER handle)
{
    return handle->logical;
}
static void WdfObjectDelete(WDFCOMMONBUFFER handle)
{
    CHECK(handle == &fake.buffer && fake.deletes == 0);
    ++fake.deletes;
}

#include "../wdf_common_buffer.h"

static void test_view(void)
{
    const size_t length = 16;
    MT_DMA_BUFFER_VIEW v = {(uint8_t *)(uintptr_t)0x1000, UINT32_MAX - 15, length};
    CHECK(mt_dma_buffer_view_valid32(&v, length, 15));
    ++v.logical;
    CHECK(!mt_dma_buffer_view_valid32(&v, length, 0));  /* Last byte crosses 4 GiB. */
    v.logical = UINT32_MAX;
    v.bytes = 1;
    CHECK(mt_dma_buffer_view_valid32(&v, 1, 0));
    v.logical = (uint64_t)UINT32_MAX + 1;
    CHECK(!mt_dma_buffer_view_valid32(&v, 1, 0));
    v.logical = 0;
    v.cpu = (uint8_t *)(UINTPTR_MAX - 15);
    v.bytes = length;
    CHECK(mt_dma_buffer_view_valid32(&v, length, 15));
    v.cpu = (uint8_t *)(UINTPTR_MAX - 14);
    CHECK(!mt_dma_buffer_view_valid32(&v, length, 0));  /* Last VA wraps. */
    v.cpu = (uint8_t *)(uintptr_t)0x1010;
    CHECK(mt_dma_buffer_view_valid32(&v, length, 15));
    CHECK(!mt_dma_buffer_view_valid32(&v, length, 63));
    v.cpu = (uint8_t *)(uintptr_t)0x1000;
    v.logical = 0x2010;
    CHECK(!mt_dma_buffer_view_valid32(&v, length, 63));
    CHECK(!mt_dma_buffer_view_valid32(&v, length, 16));  /* Bytes passed as mask. */
    CHECK(!mt_dma_buffer_view_valid32(NULL, length, 0));
    v.bytes = 0;
    CHECK(!mt_dma_buffer_view_valid32(&v, 0, 0));
#if SIZE_MAX > UINT32_MAX
    const size_t space32 = (size_t)UINT32_MAX + 1;
    v.logical = 0;
    v.bytes = space32;
    CHECK(mt_dma_buffer_view_valid32(&v, space32, 63));
    v.bytes = space32 + 1;
    CHECK(!mt_dma_buffer_view_valid32(&v, space32 + 1, 0));
#endif
}

static FAKE_BUFFER oldBuffer;
static WDFCOMMONBUFFER output;
static MT_DMA_BUFFER_VIEW view;
static FAKE_ENABLER enablerToken;

static void reset_case(void)
{
    fake.buffer.cpu = (void *)(uintptr_t)0x2000;
    fake.buffer.logical.QuadPart = 0x3000;
    fake.buffer.bytes = 64;
    fake.result = STATUS_SUCCESS;
    fake.creates = fake.deletes = 0;
    fake.enabler = NULL;
    fake.mask = 0;
    fake.requestedBytes = 0;
    output = &oldBuffer;
    view.cpu = (uint8_t *)(uintptr_t)0x4050;
    view.logical = 123;
    view.bytes = 7;
}

static void check_failure(NTSTATUS actual, NTSTATUS expected, unsigned creates, unsigned deletes)
{
    CHECK(actual == expected);
    CHECK(fake.creates == creates && fake.deletes == deletes);
    CHECK(output == &oldBuffer);
    CHECK((uintptr_t)view.cpu == 0x4050 && view.logical == 123 && view.bytes == 7);
}

static void test_allocations(void)
{
    WDFDMAENABLER enabler = &enablerToken;
    reset_case();
    CHECK(mt_dma_allocate_common_buffer32(enabler, 64, 63, &output, &view) == STATUS_SUCCESS);
    CHECK(fake.enabler == enabler && fake.requestedBytes == 64 && fake.mask == 63);
    CHECK(output == &fake.buffer && view.cpu == fake.buffer.cpu);
    CHECK(view.logical == 0x3000 && view.bytes == 64 && fake.creates == 1 && fake.deletes == 0);
    WdfObjectDelete(output);
    CHECK(fake.deletes == 1);

    reset_case();
    fake.result = STATUS_INSUFFICIENT_RESOURCES;
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 63, &output, &view),
                  STATUS_INSUFFICIENT_RESOURCES, 1, 0);

    reset_case();
    check_failure(mt_dma_allocate_common_buffer32(enabler, 0, 0, &output, &view),
                  STATUS_INVALID_PARAMETER, 0, 0);
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 64, &output, &view),
                  STATUS_INVALID_PARAMETER, 0, 0);
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, UINT32_MAX, &output, &view),
                  STATUS_INVALID_PARAMETER, 0, 0);
    check_failure(mt_dma_allocate_common_buffer32(NULL, 64, 0, &output, &view),
                  STATUS_INVALID_PARAMETER, 0, 0);
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 0, NULL, &view),
                  STATUS_INVALID_PARAMETER, 0, 0);
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 0, &output, NULL),
                  STATUS_INVALID_PARAMETER, 0, 0);
#if SIZE_MAX > UINT32_MAX
    check_failure(mt_dma_allocate_common_buffer32(enabler, (size_t)UINT32_MAX + 2, 0, &output, &view),
                  STATUS_INVALID_PARAMETER, 0, 0);
#endif

    reset_case();
    fake.buffer.bytes = 63;
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 63, &output, &view),
                  STATUS_DATA_ERROR, 1, 1);
    reset_case();
    fake.buffer.cpu = NULL;
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 63, &output, &view),
                  STATUS_DATA_ERROR, 1, 1);
    reset_case();
    fake.buffer.cpu = (void *)(uintptr_t)0x2010;
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 63, &output, &view),
                  STATUS_DATA_ERROR, 1, 1);
    reset_case();
    fake.buffer.logical.QuadPart = 0x3010;
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 63, &output, &view),
                  STATUS_DATA_ERROR, 1, 1);
    reset_case();
    fake.buffer.logical.QuadPart = UINT32_MAX - 62;
    check_failure(mt_dma_allocate_common_buffer32(enabler, 64, 0, &output, &view),
                  STATUS_DATA_ERROR, 1, 1);
}

static void test_wdf_allocation_limits(void)
{
    /* Keep the allocator simulated even at the largest legal request. */
    reset_case();
    fake.result = STATUS_INSUFFICIENT_RESOURCES;
    check_failure(mt_dma_allocate_common_buffer32(&enablerToken, UINT32_MAX,
                  PAGE_SIZE - 1, &output, &view), STATUS_INSUFFICIENT_RESOURCES, 1, 0);

    reset_case();
    const ULONG wideMask = 2 * PAGE_SIZE - 1;
    check_failure(mt_dma_allocate_common_buffer32(&enablerToken, UINT32_MAX - wideMask + 1u,
                  wideMask, &output, &view), STATUS_INVALID_PARAMETER, 0, 0);
    fake.result = STATUS_INSUFFICIENT_RESOURCES;
    check_failure(mt_dma_allocate_common_buffer32(&enablerToken, UINT32_MAX - wideMask,
                  wideMask, &output, &view), STATUS_INSUFFICIENT_RESOURCES, 1, 0);
#if SIZE_MAX > UINT32_MAX
    reset_case();
    check_failure(mt_dma_allocate_common_buffer32(&enablerToken, (size_t)UINT32_MAX + 1,
                  0, &output, &view), STATUS_INVALID_PARAMETER, 0, 0);
#endif
}

int main(void)
{
    test_view();
    test_allocations();
    test_wdf_allocation_limits();
    puts("32-bit span, caller alignment, allocation status and single-object ownership checks passed");
    return 0;
}
