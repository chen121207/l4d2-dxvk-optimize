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
repository. The helper constructs two `FgdsVkFrame` records from consecutive RT
frames and, when `DXVK_FFG_ENABLE=1`, calls the dynamically loaded
`ffgVkRecord` compute runtime without taking device or queue ownership.

The RTX 3050 probe exposes these RT extensions to a 64-bit process but not to
a 32-bit process. Since L4D2 is 32-bit, the helper boundary is required on
this driver. The current bridge uses named shared memory for CPU-visible
outputs and is a development path, not a finished production compositor. Real
L4D2 material coverage, performance, and VAC-safe deployment still require
offline testing.
