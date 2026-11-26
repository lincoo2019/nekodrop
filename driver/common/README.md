# MediaTek KMDF common buffers for 32-bit DMA descriptors

`mt_dma_allocate_common_buffer32` allocates one common buffer from the
caller's `WDFDMAENABLER`. Use `view.cpu` for CPU access and `view.logical`
for the device address. The helper validates the requested length, the
alignment of both addresses and the range through the final byte before
returning a handle. A failed call leaves both outputs unchanged.

Include `wdf_common_buffer.h` after the KMDF headers. Call the allocator at
`PASSIVE_LEVEL`. Supply the alignment as a WDF mask. For example, the MT7927
six-queue experiment requested at least 16-byte alignment.

```c
ULONG mask = max(WdfDeviceGetAlignmentRequirement(device), 15u);
WDFCOMMONBUFFER handle;
MT_DMA_BUFFER_VIEW view;
NTSTATUS status = mt_dma_allocate_common_buffer32(enabler, bytes, mask, &handle, &view);
```

On success the caller owns the handle. Keep it for as long as the device
can access the buffer. The chip-specific backend controls queue submission,
stopping DMA and handling surprise removal. If a later allocation fails
before any address has been submitted to the device, the caller can release
the handles already acquired with `WdfObjectDelete`.

On one Windows MT7927 (`14c3:7927`, subsystem `105b:e0fe`), the caller
allocated 167 buffers for six queues. Bytes held went from 0 to 5,316,848
and returned to 0 on close. All buffers stayed private to the driver during
this test. Device access and MT7925 integration still need hardware tests.
The [MT7927 reference caller](https://github.com/Yanlyn/nekodrop/blob/7fe01e534daa0d714ea68f8d4ec26622d3988c18/driver/regproxy-dma-lab/session/ring_session_buffers.h)
shows the allocation loop and rollback before extraction of this helper.
The two backend authors discussed ownership in [issue #57](https://github.com/zheznanohana/nekodrop/issues/57#issuecomment-7166512447).

Run the C11 tests from the repository root. They use a WDF test double to
exercise address boundaries, alignment, allocation failures and handle cleanup.

```sh
cmake -S driver/common/tests -B build/mtk-common
cmake --build build/mtk-common --config Release
ctest --test-dir build/mtk-common -C Release --output-on-failure
```
