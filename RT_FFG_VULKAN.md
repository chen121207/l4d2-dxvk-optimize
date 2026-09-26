# L4D2 Vulkan RT / FFG integration status

This fork now has an opt-in Vulkan ray-tracing capability gate:

    dxvk.enableRayTracing = True

When enabled, DXVK requests the Vulkan KHR acceleration-structure,
ray-tracing-pipeline, ray-query and deferred-host-operations extensions when
the physical device exposes them. The enabled device log reports a capability
message; it does not claim that an RT pass was rendered.
When the option is absent or false, the existing raster path is unchanged.

`experimental/rt` now contains a real x64 Vulkan RT implementation and a
hardware test. It builds BLAS/TLAS, runs a `VK_KHR_ray_query` compute shader,
and verifies GPU color/depth/motion/object-ID output on the RTX 3050. This is
the renderer-side proof point, not yet the L4D2 game path: the DXVK D3D9 draw
stream is not currently being converted into these BLAS/TLAS resources and no
RT result is composited into the game's swapchain yet.

The Vulkan FGDS ABI is mirrored in include/fgds/fgds_vk.h and the FreeFrameGen
repository. It carries Color, Depth, Motion, Object ID, camera metadata and
timeline-semaphore readiness without converting the Vulkan device to D3D12.
FreeFrameGen now has a separate Vulkan compute runtime that consumes this ABI;
the L4D2 producer and actual RT geometry capture are still future integration
work.

The RTX 3050 probe exposes these RT extensions to a 64-bit process but not to
a 32-bit process. Since L4D2 is 32-bit, the next integration task is a 64-bit
helper with explicit Vulkan external-memory/semaphore handoff; a 32-bit DXVK
DLL cannot safely claim to run ray queries on this driver.
