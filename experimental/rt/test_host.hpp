#pragma once
#include "rt.hpp"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <functional>
#include <filesystem>
#include <algorithm>


namespace dxvk::rt::testing {
struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
inline std::atomic<uint32_t> validationErrors{0};
inline VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
  VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
  std::cerr << "Vulkan validation: " << data->pMessage << '\n'; return VK_FALSE;
}
#define HOST_DEVICE_FUNCTIONS(X) \
 X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
 X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandPool) \
 X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkQueueSubmit) \
 X(vkDeviceWaitIdle) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdFillBuffer)
struct Host {
  HMODULE module = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr getInstance = nullptr;
  PFN_vkGetDeviceProcAddr getDevice = nullptr;
  PFN_vkDestroyInstance destroyInstance = nullptr;
  PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
  Device d;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
#define DECLARE(name) PFN_##name name = nullptr;
  HOST_DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
  ~Host() {
    if (d.handle && vkDeviceWaitIdle) vkDeviceWaitIdle(d.handle);
    if (fence) vkDestroyFence(d.handle, fence, nullptr);
    if (pool) vkDestroyCommandPool(d.handle, pool, nullptr);
    if (d.handle && vkDestroyDevice) vkDestroyDevice(d.handle, nullptr);
    if (messenger) destroyMessenger(instance, messenger, nullptr);
    if (instance) destroyInstance(instance, nullptr);
    if (module) FreeLibrary(module);
  }
  template<class T> T proc(const char* name) {
    auto result = reinterpret_cast<T>(getInstance(instance, name));
    if (!result) throw std::runtime_error(std::string("Missing instance entry: ")+name);
    return result;
  }
  void init(bool requireValidation, bool needRt = true, bool external = false) {
    module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw Unsupported("System Vulkan loader missing");
    getInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vkGetInstanceProcAddr"));
    if (!getInstance) throw Unsupported("No vkGetInstanceProcAddr");
    auto create = proc<PFN_vkCreateInstance>("vkCreateInstance");
    auto enumerateLayers = proc<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties");
    uint32_t count = 0; check(enumerateLayers(&count,nullptr),"EnumerateLayers");
    std::vector<VkLayerProperties> layers(count); check(enumerateLayers(&count,layers.data()),"EnumerateLayers");
    bool validation = std::any_of(layers.begin(),layers.end(),[](const auto& l) {
      return std::strcmp(l.layerName,"VK_LAYER_KHRONOS_validation")==0;
    });
    if (requireValidation && !validation) throw std::runtime_error("--validation required but Khronos layer is unavailable");
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* debugExt = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "DXVK RT hardware lab (not L4D2)";
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
    VkDebugUtilsMessengerCreateInfoEXT di{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
      | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT; di.pfnUserCallback = debugMessage;
    if (validation) {
      ci.enabledLayerCount=1; ci.ppEnabledLayerNames=&layer;
      ci.enabledExtensionCount=1; ci.ppEnabledExtensionNames=&debugExt; ci.pNext=&di;
    }
    check(create(&ci,nullptr,&instance),"CreateInstance");
    destroyInstance = proc<PFN_vkDestroyInstance>("vkDestroyInstance");
    std::cout << "Khronos_validation=" << (validation ? "enabled" : "unavailable (not a validation pass)") << '\n';
    if (validation) {
      destroyMessenger = proc<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT");
      check(proc<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT")(instance,&di,nullptr,&messenger),"CreateMessenger");
    }
    auto enumerate = proc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    check(enumerate(instance,&count,nullptr),"EnumerateGPU");
    std::vector<VkPhysicalDevice> devices(count); check(enumerate(instance,&count,devices.data()),"EnumerateGPU");
    auto features = proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    auto props = proc<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
    auto extensions = proc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    auto families = proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    const char* required[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,VK_KHR_RAY_QUERY_EXTENSION_NAME,
      VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    for (auto physical : devices) {
      VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
      VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p.pNext=&asProps; props(physical,&p);
      if (p.properties.apiVersion < VK_API_VERSION_1_2) continue;
      check(extensions(physical,nullptr,&count,nullptr),"EnumerateDeviceExtensions");
      std::vector<VkExtensionProperties> exts(count); check(extensions(physical,nullptr,&count,exts.data()),"EnumerateDeviceExtensions");
      bool all = true;
      std::cout << "Probe GPU=" << p.properties.deviceName << " process_bits=" << sizeof(void*)*8 << '\n';
      for (auto name : required) {
        const bool found=std::any_of(exts.begin(),exts.end(),[&](const auto& e) {return std::strcmp(e.extensionName,name)==0;});
        std::cout << "  " << name << '=' << found << '\n'; if (needRt) all &= found;
      }
      const char* sharedExtensions[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
      if (external) for (auto name : sharedExtensions) {
        const bool found=std::any_of(exts.begin(),exts.end(),[&](const auto& e) {return std::strcmp(e.extensionName,name)==0;});
        std::cout << "  " << name << '=' << found << '\n'; all &= found;
      }
      if (!all) continue;
      VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
      VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR}; as.pNext=&rq;
      VkPhysicalDeviceBufferDeviceAddressFeatures ba{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES}; ba.pNext=&as;
      VkPhysicalDeviceFeatures2 ft{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; ft.pNext=&ba; features(physical,&ft);
      std::cout << "  rayQuery=" << rq.rayQuery << " accelerationStructure=" << as.accelerationStructure
        << " bufferDeviceAddress=" << ba.bufferDeviceAddress << " storageExtended=" << ft.features.shaderStorageImageExtendedFormats << '\n';
      if ((needRt && (!rq.rayQuery || !as.accelerationStructure)) || !ba.bufferDeviceAddress || !ft.features.shaderStorageImageExtendedFormats) continue;
      families(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> queues(count); families(physical,&count,queues.data());
      uint32_t family=0;
      for (;family<count;++family) if (queues[family].queueCount && (queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) break;
      if (family==count) continue;
      if (needRt && (asProps.maxPrimitiveCount < 2000000 || !asProps.maxInstanceCount || !asProps.maxGeometryCount
        || !asProps.maxPerStageDescriptorAccelerationStructures)) continue;
      float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
      ba.bufferDeviceAddressCaptureReplay=ba.bufferDeviceAddressMultiDevice=VK_FALSE;
      as.accelerationStructureCaptureReplay=as.accelerationStructureIndirectBuild=as.accelerationStructureHostCommands
        =as.descriptorBindingAccelerationStructureUpdateAfterBind=VK_FALSE;
      ft.features={}; ft.features.shaderStorageImageExtendedFormats=VK_TRUE;
      if (!needRt) ba.pNext=nullptr;
      VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pNext=&ft; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qi;
      std::vector<const char*> enabled;
      if (needRt) enabled.insert(enabled.end(),std::begin(required),std::end(required));
      if (external) enabled.insert(enabled.end(),std::begin(sharedExtensions),std::end(sharedExtensions));
      dc.enabledExtensionCount=uint32_t(enabled.size()); dc.ppEnabledExtensionNames=enabled.data();
      check(proc<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dc,nullptr,&d.handle),"CreateDevice");
      d.physical=physical; d.family=family; d.scratchAlignment=needRt ? asProps.minAccelerationStructureScratchOffsetAlignment : 1;
      d.limits=p.properties.limits;
      std::cout << "GPU=" << p.properties.deviceName << " RT_enabled=" << needRt << " addressBits=" << sizeof(void*)*8 << '\n';
      break;
    }
    if (!d.handle) throw Unsupported("No Vulkan 1.2 GPU with rayQuery, AS and required storage formats");
    getDevice = proc<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
#define LOAD(name) name = reinterpret_cast<PFN_##name>(getDevice(d.handle,#name)); \
    if (!name) throw std::runtime_error("Missing device entry: " #name);
    HOST_DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    proc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(d.physical,&d.memory);
    d.load(getDevice,needRt); vkGetDeviceQueue(d.handle,d.family,0,&queue);
    // Validate optimal storage/readback formats and BLAS vertex format explicitly.
    auto formatProps = proc<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
    for (auto f : {VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_UINT}) {
      VkFormatProperties p{}; formatProps(d.physical,f,&p);
      const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if ((p.optimalTilingFeatures & needed)!=needed) throw Unsupported("Missing optimal image format support");
    }
    VkFormatProperties vp{}; formatProps(d.physical,VK_FORMAT_R32G32B32_SFLOAT,&vp);
    if (needRt && !(vp.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR)) throw Unsupported("Missing BLAS vertex format");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex=d.family;
    check(vkCreateCommandPool(d.handle,&cp,nullptr,&pool),"CreateCommandPool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    check(vkAllocateCommandBuffers(d.handle,&ca,&cmd),"AllocateCommandBuffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(d.handle,&fi,nullptr,&fence),"CreateFence");
  }
  void run(const std::function<void(VkCommandBuffer)>& record,
           VkSemaphore waitSemaphore = VK_NULL_HANDLE, VkSemaphore signalSemaphore = VK_NULL_HANDLE) {
    check(vkResetCommandPool(d.handle,pool,0),"ResetCommandPool");
    check(vkResetFences(d.handle,1,&fence),"ResetFence");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd,&bi),"BeginCommandBuffer"); record(cmd); check(vkEndCommandBuffer(cmd),"EndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
    const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (waitSemaphore) { submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&waitSemaphore;submit.pWaitDstStageMask=&waitStage; }
    if (signalSemaphore) { submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&signalSemaphore; }
    check(vkQueueSubmit(queue,1,&submit,fence),"QueueSubmit");
    check(vkWaitForFences(d.handle,1,&fence,VK_TRUE,30'000'000'000ull),"WaitFence (test host only)");
  }
  std::vector<uint8_t> read(Image& image, uint32_t pixelBytes) {
    Buffer readback(d,VkDeviceSize(image.width)*image.height*pixelBytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    run([&](VkCommandBuffer c) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.oldLayout=VK_IMAGE_LAYOUT_GENERAL; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      b.image=image.handle; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      d.vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
      VkBufferImageCopy region{}; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent={image.width,image.height,1};
      vkCmdCopyImageToBuffer(c,image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&region);
      std::swap(b.oldLayout,b.newLayout); b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      d.vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
      memoryBarrier(d,c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    });
    std::vector<uint8_t> bytes(size_t(readback.size)); std::memcpy(bytes.data(),readback.mapped,bytes.size()); return bytes;
  }
};

} // namespace dxvk::rt::testing
