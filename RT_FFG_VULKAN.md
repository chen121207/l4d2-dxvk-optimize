# L4D2 Vulkan RT / FFG integration status

This fork now has an opt-in Vulkan ray-tracing capability gate:

    dxvk.enableRayTracing = True

When enabled, DXVK starts the x64 `rt_helper.exe` bridge if present. The 32-bit
DXVK DLL captures supported D3D9 geometry/camera data, the helper renders the
Vulkan RT pass, and the result is copied into the ordinary DXVK backbuffer
before Present. Missing helper, unsupported draws, or a failed frame leave the
normal raster path in place.
When the option is absent or false, the existing raster path is unchanged.

`experimental/rt` contains a real x64 Vulkan RT implementation and a hardware
test. It builds BLAS/TLAS, runs a `VK_KHR_ray_query` compute shader, and
verifies GPU color/depth/motion/object-ID output on the RTX 3050. The bridge
test also proves the x86-to-x64 process boundary and the optional second-frame
FFG record path.

The Vulkan FGDS ABI is mirrored in include/fgds/fgds_vk.h and the FreeFrameGen
repository. The helper maps its typed Vulkan formats to the protocol IDs,
queries `ffgVkGetCapabilities`, and prefers the append-only V2 contract with
CORE resource flags and `ffgVkRecordV2`. It constructs two frame records from
consecutive RT frames and records the reverse-motion pass for the older
endpoint, as required by the bidirectional-motion rule. A runtime exposing
only the frozen V1 entry points remains supported as a compatibility fallback.

This is helper-native FFG: both the RT images and FFG command recording are on
the helper's Vulkan device. The final result still crosses the existing
x86-to-x64 CPU bridge. It is not game-native FFG. A true in-process path needs
an x86 FFG DLL, DXVK-owned RGBA32F/R32F/RG32F/R32UI storage images, a command
buffer integration point, and raster motion/Object-ID resources. The x64 FFG
DLL must not be loaded into the 32-bit L4D2 process.

The RTX 3050 probe exposes these RT extensions to a 64-bit process but not to
a 32-bit process. Since L4D2 is 32-bit, the helper boundary is required on
this driver. The current bridge uses named shared memory for CPU-visible
outputs and is a development path, not a finished production compositor. Real
L4D2 material coverage, performance, and VAC-safe deployment still require
offline testing.
