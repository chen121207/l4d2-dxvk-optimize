# Vulkan RT hardware lab

This directory contains the first real GPU ray-tracing implementation used by
the L4D2 renderer work. It is intentionally a separate x64 target until the
game integration is proven.

The test builds a bottom-level acceleration structure from triangles, a
top-level instance, then records a compute shader using `VK_KHR_ray_query`.
The shader emits:

- RT color with optional hard shadows and one diagnostic reflection bounce;
- linear depth;
- motion vectors between two camera poses;
- object IDs, with zero reserved for a miss.

The command recording path does not submit, wait, or present. The host test
owns the Vulkan device and verifies the GPU image against a small CPU oracle.
That separation is the same boundary needed by DXVK and FFG, but this target
does not yet intercept L4D2's D3D9 draw stream or replace its swapchain.

## Build and run

From the `dxvk-optimize` checkout:

```powershell
cmake -S experimental/rt -B build-rt-x64 -G "Visual Studio 18 2026" -A x64
cmake --build build-rt-x64 --config Release --parallel 4
build-rt-x64/Release/rt_test.exe --output artifacts/rt-test
```

The hardware test returns `77` only when no Vulkan device exposes the required
ray-query, acceleration-structure, buffer-address, and storage-image features.
It does not silently fall back to CPU tracing.
