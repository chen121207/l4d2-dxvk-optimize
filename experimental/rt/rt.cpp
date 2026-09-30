#include "rt.hpp"
#include "rt_shader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dxvk::rt {
void Device::load(PFN_vkGetDeviceProcAddr getProc, bool rayTracing) {
  if (!getProc || !handle || !physical || !scratchAlignment)
    throw std::runtime_error("Invalid RT device configuration");
#define LOAD(name) name = reinterpret_cast<PFN_##name>(getProc(handle, #name)); \
  if (!name && (rayTracing || !std::strstr(#name,"AccelerationStructure"))) \
    throw std::runtime_error("Missing device entry point: " #name);
  RT_DEVICE_FUNCTIONS(LOAD)
#undef LOAD
}
uint32_t Device::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
  throw std::runtime_error("No compatible memory type");
}
Buffer::Buffer(Device& d, VkDeviceSize bytes, VkBufferUsageFlags usage, bool host) : device(&d), size(bytes) {
  if (!bytes) throw std::runtime_error("Empty buffer");
  try {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(d.vkCreateBuffer(d.handle, &info, nullptr, &handle), "CreateBuffer");
    VkMemoryRequirements requirements{};
    d.vkGetBufferMemoryRequirements(d.handle, handle, &requirements);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) allocation.pNext = &flags;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = d.memoryType(requirements.memoryTypeBits, host
      ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(d.vkAllocateMemory(d.handle, &allocation, nullptr, &memory), "AllocateBufferMemory");
    check(d.vkBindBufferMemory(d.handle, handle, memory, 0), "BindBufferMemory");
    if (host) check(d.vkMapMemory(d.handle, memory, 0, bytes, 0, &mapped), "MapBuffer");
  } catch (...) {
    if (handle) d.vkDestroyBuffer(d.handle, handle, nullptr);
    if (memory) d.vkFreeMemory(d.handle, memory, nullptr);
    throw;
  }
}
Buffer::~Buffer() {
  if (mapped) device->vkUnmapMemory(device->handle, memory);
  if (handle) device->vkDestroyBuffer(device->handle, handle, nullptr);
  if (memory) device->vkFreeMemory(device->handle, memory, nullptr);
}
VkDeviceAddress Buffer::address() const {
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = handle;
  const auto address = device->vkGetBufferDeviceAddress(device->handle, &info);
  if (!address) throw std::runtime_error("Zero GPU buffer address");
  return address;
}
Image::Image(Device& d, uint32_t w, uint32_t h, VkFormat f) : device(&d), format(f), width(w), height(h) {
  if (!w || !h || w > d.limits.maxImageDimension2D || h > d.limits.maxImageDimension2D)
    throw std::runtime_error("Invalid RT image extent");
  try {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = f; info.extent = {w, h, 1};
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    check(d.vkCreateImage(d.handle, &info, nullptr, &handle), "CreateImage");
    VkMemoryRequirements requirements{};
    d.vkGetImageMemoryRequirements(d.handle, handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = d.memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(d.vkAllocateMemory(d.handle, &allocation, nullptr, &memory), "AllocateImageMemory");
    check(d.vkBindImageMemory(d.handle, handle, memory, 0), "BindImageMemory");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = handle; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(d.vkCreateImageView(d.handle, &vi, nullptr, &view), "CreateImageView");
  } catch (...) {
    if (view) d.vkDestroyImageView(d.handle, view, nullptr);
    if (handle) d.vkDestroyImage(d.handle, handle, nullptr);
    if (memory) d.vkFreeMemory(d.handle, memory, nullptr);
    throw;
  }
}
Image::~Image() {
  if (view) device->vkDestroyImageView(device->handle, view, nullptr);
  if (handle) device->vkDestroyImage(device->handle, handle, nullptr);
  if (memory) device->vkFreeMemory(device->handle, memory, nullptr);
}
FgdsVkImage Image::fgds() const { return {encode(handle), encode(view), uint32_t(format), VK_IMAGE_LAYOUT_GENERAL}; }
Frame::Frame(Device& d, uint32_t w, uint32_t h) {
  constexpr VkFormat formats[] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32_UINT};
  for (size_t i = 0; i < 4; ++i) images[i] = std::make_unique<Image>(d, w, h, formats[i]);
}
void Frame::initialize(VkCommandBuffer cmd) {
  auto& d = *images[0]->device;
  std::array<VkImageMemoryBarrier, 4> barriers{};
  for (size_t i = 0; i < 4; ++i) {
    auto& b = barriers[i]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.image = images[i]->handle; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  d.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0, 0, nullptr, 0, nullptr, 4, barriers.data());
}
FgdsVkFrame Frame::metadata(uint64_t id, uint64_t timeNs, const Camera& c) const {
  const auto& d = *images[0]->device;
  FgdsVkFrame f{};
  f.structSize = sizeof(f); f.version = FGDS_VK_VERSION;
  f.width = images[0]->width; f.height = images[0]->height;
  f.frameId = id; f.timestampNs = timeNs; f.device = encode(d.handle);
  f.physicalDevice = encode(d.physical); f.queueFamily = d.family;
  f.color = images[0]->fgds(); f.depth = images[1]->fgds();
  f.motionToOther = images[2]->fgds(); f.objectId = images[3]->fgds();
  // Column-major Vulkan [0,1] clip Z; Y is downward. Linear depth is separate.
  const Vec3 x{c.right.x / (c.tanHalfFov*c.aspect), c.right.y / (c.tanHalfFov*c.aspect), c.right.z / (c.tanHalfFov*c.aspect)};
  const Vec3 y{-c.up.x / c.tanHalfFov, -c.up.y / c.tanHalfFov, -c.up.z / c.tanHalfFov};
  const float zScale = c.farPlane / (c.farPlane - .01f);
  const auto dot = [](Vec3 a, Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;};
  float matrix[] = {x.x,y.x,c.forward.x*zScale,c.forward.x,
    x.y,y.y,c.forward.y*zScale,c.forward.y, x.z,y.z,c.forward.z*zScale,c.forward.z,
    -dot(x,c.origin),-dot(y,c.origin),(-dot(c.forward,c.origin)-.01f)*zScale,-dot(c.forward,c.origin)};
  std::copy(std::begin(matrix), std::end(matrix), f.worldToClip);
  return f;
}
void memoryBarrier(Device& d, VkCommandBuffer cmd, VkPipelineStageFlags source, VkAccessFlags sourceAccess,
                   VkPipelineStageFlags dest, VkAccessFlags destAccess) {
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  b.srcAccessMask = sourceAccess; b.dstAccessMask = destAccess;
  d.vkCmdPipelineBarrier(cmd, source, dest, 0, 1, &b, 0, nullptr, 0, nullptr);
}
namespace {
void validateCamera(const Camera& c) {
  auto finite = [](Vec3 v) {return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);};
  auto dot = [](Vec3 a, Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;};
  if (!finite(c.origin) || !finite(c.forward) || !finite(c.right) || !finite(c.up)
    || !std::isfinite(c.tanHalfFov) || c.tanHalfFov <= 0 || !std::isfinite(c.aspect) || c.aspect <= 0
    || !std::isfinite(c.farPlane) || c.farPlane <= .01f || (c.flags & ~7u)
    || std::abs(dot(c.forward,c.forward)-1) > .001f || std::abs(dot(c.right,c.right)-1) > .001f
    || std::abs(dot(c.up,c.up)-1) > .001f || std::abs(dot(c.right,c.forward)) > .001f
    || std::abs(dot(c.up,c.forward)) > .001f || std::abs(dot(c.right,c.up)) > .001f)
    throw std::runtime_error("Invalid/non-orthonormal RT camera");
}
struct Acceleration {
  Device& d;
  VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
  std::unique_ptr<Buffer> storage, scratch;
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  VkAccelerationStructureBuildRangeInfoKHR range{};
  explicit Acceleration(Device& device) : d(device) {}
  ~Acceleration() { if (handle) d.vkDestroyAccelerationStructureKHR(d.handle, handle, nullptr); }
  void create(VkAccelerationStructureTypeKHR type, uint32_t count) {
    range.primitiveCount = count; build.type = type;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1; build.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    d.vkGetAccelerationStructureBuildSizesKHR(d.handle, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count, &sizes);
    storage = std::make_unique<Buffer>(d, sizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    scratch = std::make_unique<Buffer>(d, sizes.buildScratchSize + d.scratchAlignment,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = storage->handle; ci.size = sizes.accelerationStructureSize; ci.type = type;
    check(d.vkCreateAccelerationStructureKHR(d.handle, &ci, nullptr, &handle), "CreateAccelerationStructure");
    build.dstAccelerationStructure = handle;
    auto address = scratch->address(); const uint64_t alignment = d.scratchAlignment;
    build.scratchData.deviceAddress = (address + alignment - 1) / alignment * alignment;
  }
  void record(VkCommandBuffer cmd) {
    const auto* r = &range;
    d.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &r);
  }
};
}
struct Scene::Impl {
  Device& d;
  std::unique_ptr<Buffer> vertices, triangles, instances;
  Acceleration blas, tlas;
  VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 3> descriptors{};
  bool buildRecorded = false;
  explicit Impl(Device& device) : d(device), blas(device), tlas(device) {}
  ~Impl() {
    if (pool) d.vkDestroyDescriptorPool(d.handle, pool, nullptr);
    if (pipeline) d.vkDestroyPipeline(d.handle, pipeline, nullptr);
    if (pipelineLayout) d.vkDestroyPipelineLayout(d.handle, pipelineLayout, nullptr);
    if (descriptorLayout) d.vkDestroyDescriptorSetLayout(d.handle, descriptorLayout, nullptr);
  }
};
Scene::Scene(Device& d, const std::vector<Triangle>& mesh) : impl(std::make_unique<Impl>(d)) {
  if (mesh.empty() || mesh.size() > 2000000 || mesh.size()*sizeof(Triangle) > d.limits.maxStorageBufferRange)
    throw std::runtime_error("RT mesh outside safety/device limits");
  if (d.limits.maxPushConstantsSize < 128 || d.limits.maxPerStageDescriptorStorageImages < 4)
    throw std::runtime_error("Insufficient RT pipeline limits");
  // D3D9 streams commonly contain zero-area UI/particle primitives. They are
  // legal to submit to the rasterizer but Vulkan acceleration structures reject
  // them. Drop only those triangles instead of rejecting the whole frame.
  std::vector<Triangle> filtered;
  filtered.reserve(mesh.size());
  for (const auto& t : mesh) {
    if (!t.id) throw std::runtime_error("Object ID 0 is reserved for invalid/miss pixels");
    for (const auto p : {t.a,t.b,t.c,t.albedo})
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        throw std::runtime_error("Nonfinite triangle");
    Vec3 a{t.b.x-t.a.x,t.b.y-t.a.y,t.b.z-t.a.z}, b{t.c.x-t.a.x,t.c.y-t.a.y,t.c.z-t.a.z};
    Vec3 n{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
    if (n.x*n.x+n.y*n.y+n.z*n.z < 1e-12f) continue;
    filtered.push_back(t);
  }
  if (filtered.empty()) throw std::runtime_error("RT scene contains no non-degenerate triangles");
  auto& s = *impl;
  std::vector<Vec3> points; points.reserve(filtered.size()*3);
  for (const auto& t : filtered)
    points.insert(points.end(), {t.a,t.b,t.c});
  const VkBufferUsageFlags inputUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  s.vertices = std::make_unique<Buffer>(d, points.size()*sizeof(Vec3), inputUsage, true);
  std::memcpy(s.vertices->mapped, points.data(), size_t(s.vertices->size));
  s.triangles = std::make_unique<Buffer>(d, filtered.size()*sizeof(Triangle), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
  std::memcpy(s.triangles->mapped, filtered.data(), size_t(s.triangles->size));
  auto& g = s.blas.geometry;
  g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  g.geometry.triangles.vertexData.deviceAddress = s.vertices->address();
  g.geometry.triangles.vertexStride = sizeof(Vec3); g.geometry.triangles.maxVertex = uint32_t(points.size()-1);
  g.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
  s.blas.create(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, uint32_t(filtered.size()));
  VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  ai.accelerationStructure = s.blas.handle;
  VkAccelerationStructureInstanceKHR instance{};
  instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
  instance.mask = 255; instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
  instance.accelerationStructureReference = d.vkGetAccelerationStructureDeviceAddressKHR(d.handle, &ai);
  if (!instance.accelerationStructureReference) throw std::runtime_error("Zero BLAS address");
  s.instances = std::make_unique<Buffer>(d, sizeof(instance), inputUsage, true);
  std::memcpy(s.instances->mapped, &instance, sizeof(instance));
  s.tlas.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  s.tlas.geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  s.tlas.geometry.geometry.instances.data.deviceAddress = s.instances->address();
  s.tlas.create(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 1);
  VkDescriptorSetLayoutBinding bindings[6]{};
  for (uint32_t i = 0; i < 6; ++i) {
    bindings[i].binding = i; bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
      : i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  }
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = 6; li.pBindings = bindings;
  check(d.vkCreateDescriptorSetLayout(d.handle, &li, nullptr, &s.descriptorLayout), "CreateRTDescriptorLayout");
  VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
  VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pi.setLayoutCount = 1; pi.pSetLayouts = &s.descriptorLayout; pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &pc;
  check(d.vkCreatePipelineLayout(d.handle, &pi, nullptr, &s.pipelineLayout), "CreateRTPipelineLayout");
  VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = sizeof(rt_shader); si.pCode = rt_shader;
  VkShaderModule shader = VK_NULL_HANDLE;
  check(d.vkCreateShaderModule(d.handle, &si, nullptr, &shader), "CreateRTShader");
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = shader; ci.stage.pName = "main";
  ci.layout = s.pipelineLayout;
  auto result = d.vkCreateComputePipelines(d.handle, VK_NULL_HANDLE, 1, &ci, nullptr, &s.pipeline);
  d.vkDestroyShaderModule(d.handle, shader, nullptr); check(result, "CreateRTPipeline");
  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,3},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,12}};
  VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  di.maxSets = 3; di.poolSizeCount = 3; di.pPoolSizes = sizes;
  check(d.vkCreateDescriptorPool(d.handle, &di, nullptr, &s.pool), "CreateRTDescriptorPool");
  VkDescriptorSetLayout layouts[] = {s.descriptorLayout,s.descriptorLayout,s.descriptorLayout};
  VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocation.descriptorPool = s.pool; allocation.descriptorSetCount = 3; allocation.pSetLayouts = layouts;
  check(d.vkAllocateDescriptorSets(d.handle, &allocation, s.descriptors.data()), "AllocateRTDescriptorSets");
}
Scene::~Scene() = default;
void Scene::recordBuild(VkCommandBuffer cmd) {
  auto& s = *impl;
  if (!cmd || s.buildRecorded) throw std::runtime_error("Scene build already recorded or null command buffer");
  memoryBarrier(s.d,cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,
    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
  s.blas.record(cmd);
  memoryBarrier(s.d,cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  s.tlas.record(cmd);
  memoryBarrier(s.d,cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  s.buildRecorded = true;
}
void Scene::recordTrace(VkCommandBuffer cmd, Frame& f, const Camera& current, const Camera& other, uint32_t slot) {
  auto& s = *impl;
  if (!cmd || !s.buildRecorded || slot >= 3) throw std::runtime_error("Invalid trace recording order/slot");
  validateCamera(current); validateCamera(other);
  VkDescriptorImageInfo images[4]{};
  for (size_t i = 0; i < 4; ++i) {
    if (f.images[i]->device != &s.d) throw std::runtime_error("RT frame belongs to another device");
    images[i].imageView = f.images[i]->view; images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }
  const auto w = f.images[0]->width, h = f.images[0]->height;
  if ((w+7)/8 > s.d.limits.maxComputeWorkGroupCount[0] || (h+7)/8 > s.d.limits.maxComputeWorkGroupCount[1])
    throw std::runtime_error("RT dispatch exceeds device limit");
  VkDescriptorBufferInfo buffer{s.triangles->handle,0,s.triangles->size};
  VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  as.accelerationStructureCount = 1; as.pAccelerationStructures = &s.tlas.handle;
  VkWriteDescriptorSet writes[6]{};
  for (uint32_t i = 0; i < 6; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = s.descriptors[slot]; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
    writes[i].descriptorType = i==0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
      : i==1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (i==0) writes[i].pNext = &as;
    else if (i==1) writes[i].pBufferInfo = &buffer;
    else writes[i].pImageInfo = &images[i-2];
  }
  s.d.vkUpdateDescriptorSets(s.d.handle,6,writes,0,nullptr);
  s.d.vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline);
  s.d.vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipelineLayout,0,1,&s.descriptors[slot],0,nullptr);
  const Camera cameras[] = {current,other};
  s.d.vkCmdPushConstants(cmd,s.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(cameras),cameras);
  s.d.vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);
  memoryBarrier(s.d,cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
} // namespace dxvk::rt
