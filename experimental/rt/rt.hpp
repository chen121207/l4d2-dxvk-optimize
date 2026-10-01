#pragma once
#include <vulkan/vulkan.h>
#include <fgds/fgds_vk.h>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Experimental reusable GPU renderer. Not hooked into D3D9 or game Present.
namespace dxvk::rt {
inline void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
}
template<class T> uint64_t encode(T handle) {
  if constexpr (std::is_pointer_v<T>) return uint64_t(reinterpret_cast<uintptr_t>(handle));
  else return uint64_t(handle);
}
struct Vec3 { float x, y, z; };
struct Triangle {
  Vec3 a; uint32_t id;
  Vec3 b; uint32_t pad0 = 0;
  Vec3 c; uint32_t pad1 = 0;
  Vec3 albedo{0.6f, 0.6f, 0.6f}; uint32_t pad2 = 0;
};
static_assert(sizeof(Triangle) == 64);
enum Flags : uint32_t { Trace = 1, Shadows = 2, Reflections = 4 };
// Orthonormal camera, image origin top-left. +forward depth in world units.
struct Camera {
  Vec3 origin{0, 0, 0}; float tanHalfFov = 1;
  Vec3 forward{1, 0, 0}; float aspect = 1;
  Vec3 right{0, 1, 0}; uint32_t flags = Trace | Shadows;
  Vec3 up{0, 0, 1}; float farPlane = 10000;
};
static_assert(sizeof(Camera) == 64);

#define RT_DEVICE_FUNCTIONS(X) \
 X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
 X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
 X(vkGetBufferDeviceAddress) X(vkCreateImage) X(vkDestroyImage) \
 X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
 X(vkCreateAccelerationStructureKHR) X(vkDestroyAccelerationStructureKHR) \
 X(vkGetAccelerationStructureBuildSizesKHR) X(vkGetAccelerationStructureDeviceAddressKHR) \
 X(vkCmdBuildAccelerationStructuresKHR) X(vkCmdPipelineBarrier) \
 X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) \
 X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
 X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule) \
 X(vkDestroyShaderModule) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
 X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch)
struct Device {
  VkDevice handle = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  uint32_t family = 0;
  uint32_t scratchAlignment = 0;
  VkPhysicalDeviceLimits limits{};
  VkPhysicalDeviceMemoryProperties memory{};
#define DECLARE(name) PFN_##name name = nullptr;
  RT_DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
  // Requires enabled bufferDeviceAddress, accelerationStructure, rayQuery,
  // shaderStorageImageExtendedFormats, compute queue, and Vulkan >= 1.2.
  void load(PFN_vkGetDeviceProcAddr getProc, bool rayTracing = true);
  uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
};
struct Buffer {
  Device* device = nullptr;
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* mapped = nullptr;
  Buffer(Device&, VkDeviceSize, VkBufferUsageFlags, bool hostVisible = false);
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  VkDeviceAddress address() const;
};
struct Image {
  Device* device = nullptr;
  VkImage handle = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format;
  uint32_t width, height;
  Image(Device&, uint32_t width, uint32_t height, VkFormat);
  ~Image();
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  FgdsVkImage fgds() const;
};
struct Frame {
  std::array<std::unique_ptr<Image>, 4> images;
  Frame(Device&, uint32_t width, uint32_t height);
  void initialize(VkCommandBuffer); // Once, undefined -> GENERAL; caller submits.
  FgdsVkFrame metadata(uint64_t id, uint64_t timeNs, const Camera&) const;
  FgdsVkFrameV2 metadataV2(uint64_t id, uint64_t timeNs, const Camera&) const;
};
// Caller owns Device and command buffer. Must fence-complete all uses before
// destruction. No implicit submit, CPU ray tracing, or queue/device idle waits.
class Scene {
public:
  Scene(Device&, const std::vector<Triangle>&);
  ~Scene();
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;
  void recordBuild(VkCommandBuffer); // Once per scene; build must execute before render.
  // Descriptor slot index must be < 3. One distinct slot for each in-flight
  // recording; only reuse a slot after its GPU work completes.
  void recordTrace(VkCommandBuffer, Frame&, const Camera& current,
                   const Camera& other, uint32_t slot = 0);
private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
void memoryBarrier(Device&, VkCommandBuffer, VkPipelineStageFlags source,
                   VkAccessFlags sourceAccess, VkPipelineStageFlags dest,
                   VkAccessFlags destAccess);
} // namespace dxvk::rt
