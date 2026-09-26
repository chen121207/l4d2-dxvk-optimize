# L4D2 Vulkan RT / FFG integration status

This fork now has an opt-in Vulkan ray-tracing capability gate:

    dxvk.enableRayTracing = True

When enabled, DXVK requests the Vulkan KHR acceleration-structure,
ray-tracing-pipeline, ray-query and deferred-host-operations extensions when
the physical device exposes them. The enabled device log reports a capability
message; it does not claim that an RT pass was rendered.
When the option is absent or false, the existing raster path is unchanged.

This switch is deliberately not an RT renderer yet. The next implementation
stage must build BLAS/TLAS from the D3D9 draw/resource stream, add a ray
tracing pipeline and composite its result into the existing swapchain image.
Until those stages are complete, enabling the option only validates device
support and must not be described as game-ready ray tracing.

The Vulkan FGDS ABI is mirrored in include/fgds/fgds_vk.h and the FreeFrameGen
repository. It carries Color, Depth, Motion, Object ID, camera metadata and
timeline-semaphore readiness without converting the Vulkan device to D3D12.
FreeFrameGen now has a separate Vulkan compute runtime that consumes this ABI;
the L4D2 producer and actual RT geometry capture are still future integration
work.
