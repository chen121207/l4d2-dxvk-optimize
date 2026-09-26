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

The cross-bitness proof uses a 32-bit client and a 64-bit helper. Build both
architectures, place `rt_bridge_client.exe` and the x64 `rt_helper.exe` in the
same temporary directory, and run the client:

```powershell
cmake --build build-rt-x64 --config Release --target rt_helper --parallel 4
cmake --build build-rt-x86 --config Release --target rt_bridge_client --parallel 4
New-Item -ItemType Directory -Force bridge-run | Out-Null
Copy-Item build-rt-x86/Release/rt_bridge_client.exe bridge-run/
Copy-Item build-rt-x64/Release/rt_helper.exe bridge-run/
& .\bridge-run\rt_bridge_client.exe
```

`rt_bridge_client` creates a named shared mapping and events, uploads a small
triangle scene from a 32-bit process, then asks the 64-bit helper to execute
the real Vulkan RT shader. A `PASS cross-bitness RT bridge` result proves the
process boundary and GPU result path, not L4D2 integration.

The hardware test returns `77` only when no Vulkan device exposes the required
ray-query, acceleration-structure, buffer-address, and storage-image features.
It does not silently fall back to CPU tracing.

## Current L4D2 process boundary

The test was run in both target bitnesses on the development RTX 3050. The
64-bit process exposes `VK_KHR_acceleration_structure` and
`VK_KHR_ray_query`; the 32-bit process exposes neither. L4D2 is a 32-bit
process, so a 32-bit `d3d9.dll` cannot create a Vulkan ray-tracing device on
this driver. The game integration therefore needs a 64-bit helper/renderer
boundary (with explicit external-memory and semaphore ownership) before its
swapchain can be replaced. The current target deliberately stops at the
hardware and bridge proof point instead of pretending that a 32-bit capability
gate is game ray tracing. The next integration step is to feed DXVK's captured
scene/camera data into this helper and copy its result into the normal DXVK
present path; that work is not yet enabled by default.
