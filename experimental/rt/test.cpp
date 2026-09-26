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

using namespace dxvk::rt;
namespace {
struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
std::atomic<uint32_t> validationErrors{0};
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
  VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
  std::cerr << "Vulkan validation: " << data->pMessage << '\n'; return VK_FALSE;
}
#define HOST_DEVICE_FUNCTIONS(X) \
 X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
 X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandPool) \
 X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkQueueSubmit) \
 X(vkDeviceWaitIdle) X(vkCmdCopyImageToBuffer)
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
  void init(bool requireValidation) {
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
      for (auto name : required) all &= std::any_of(exts.begin(),exts.end(),[&](const auto& e) {return std::strcmp(e.extensionName,name)==0;});
      if (!all) continue;
      VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
      VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR}; as.pNext=&rq;
      VkPhysicalDeviceBufferDeviceAddressFeatures ba{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES}; ba.pNext=&as;
      VkPhysicalDeviceFeatures2 ft{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; ft.pNext=&ba; features(physical,&ft);
      if (!rq.rayQuery || !as.accelerationStructure || !ba.bufferDeviceAddress || !ft.features.shaderStorageImageExtendedFormats) continue;
      families(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> queues(count); families(physical,&count,queues.data());
      uint32_t family=0;
      for (;family<count;++family) if (queues[family].queueCount && (queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) break;
      if (family==count) continue;
      if (asProps.maxPrimitiveCount < 2000000 || !asProps.maxInstanceCount || !asProps.maxGeometryCount
        || !asProps.maxPerStageDescriptorAccelerationStructures) continue;
      float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
      ba.bufferDeviceAddressCaptureReplay=ba.bufferDeviceAddressMultiDevice=VK_FALSE;
      as.accelerationStructureCaptureReplay=as.accelerationStructureIndirectBuild=as.accelerationStructureHostCommands
        =as.descriptorBindingAccelerationStructureUpdateAfterBind=VK_FALSE;
      ft.features={}; ft.features.shaderStorageImageExtendedFormats=VK_TRUE;
      VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pNext=&ft; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qi;
      dc.enabledExtensionCount=3; dc.ppEnabledExtensionNames=required;
      check(proc<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dc,nullptr,&d.handle),"CreateDevice");
      d.physical=physical; d.family=family; d.scratchAlignment=asProps.minAccelerationStructureScratchOffsetAlignment;
      d.limits=p.properties.limits;
      std::cout << "GPU=" << p.properties.deviceName << " rayQuery=1 accelerationStructure=1 addressBits=" << sizeof(void*)*8 << '\n';
      break;
    }
    if (!d.handle) throw Unsupported("No Vulkan 1.2 GPU with rayQuery, AS and required storage formats");
    getDevice = proc<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
#define LOAD(name) name = reinterpret_cast<PFN_##name>(getDevice(d.handle,#name)); \
    if (!name) throw std::runtime_error("Missing device entry: " #name);
    HOST_DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    proc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(d.physical,&d.memory);
    d.load(getDevice); vkGetDeviceQueue(d.handle,d.family,0,&queue);
    // Validate optimal storage/readback formats and BLAS vertex format explicitly.
    auto formatProps = proc<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
    for (auto f : {VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_UINT}) {
      VkFormatProperties p{}; formatProps(d.physical,f,&p);
      const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if ((p.optimalTilingFeatures & needed)!=needed) throw Unsupported("Missing optimal image format support");
    }
    VkFormatProperties vp{}; formatProps(d.physical,VK_FORMAT_R32G32B32_SFLOAT,&vp);
    if (!(vp.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR)) throw Unsupported("Missing BLAS vertex format");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex=d.family;
    check(vkCreateCommandPool(d.handle,&cp,nullptr,&pool),"CreateCommandPool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    check(vkAllocateCommandBuffers(d.handle,&ca,&cmd),"AllocateCommandBuffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(d.handle,&fi,nullptr,&fence),"CreateFence");
  }
  void run(const std::function<void(VkCommandBuffer)>& record) {
    check(vkResetCommandPool(d.handle,pool,0),"ResetCommandPool");
    check(vkResetFences(d.handle,1,&fence),"ResetFence");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd,&bi),"BeginCommandBuffer"); record(cmd); check(vkEndCommandBuffer(cmd),"EndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
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
Vec3 sub(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(Vec3 a,float k) {return {a.x*k,a.y*k,a.z*k};}
Vec3 add(Vec3 a,Vec3 b) {return {a.x+b.x,a.y+b.y,a.z+b.z};}
float dot(Vec3 a,Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b) {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 normalize(Vec3 a) {return mul(a,1/std::sqrt(dot(a,a)));}
// Reference ONLY for small automated test. Never part of production rendering.
uint32_t oracle(const std::vector<Triangle>& mesh,Vec3 origin,Vec3 ray,float& t) {
  uint32_t id=0;
  for (auto tri : mesh) {
    Vec3 e1=sub(tri.b,tri.a),e2=sub(tri.c,tri.a),p=cross(ray,e2);
    float det=dot(e1,p); if (std::abs(det)<1e-8f) continue;
    Vec3 s=sub(origin,tri.a);float u=dot(s,p)/det; if (u<0 || u>1) continue;
    Vec3 q=cross(s,e1);float v=dot(ray,q)/det; if (v<0 || u+v>1) continue;
    float z=dot(e2,q)/det; if (z>.01f && z<t) {t=z;id=tri.id;}
  }
  return id;
}
std::vector<Triangle> fixture() {
  std::vector<Triangle> out;
  auto quad=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 e,uint32_t id) {
    out.push_back({a,id,b,0,c}); out.push_back({a,id,c,0,e});
  };
  quad({10,-30,-30},{10,30,-30},{10,30,30},{10,-30,30},1);
  quad({5,-1,-1},{5,1,-1},{5,1,1},{5,-1,1},2);
  return out;
}
template<class T> T pixel(const std::vector<uint8_t>& data,size_t index) {
  T value; std::memcpy(&value,data.data()+index*sizeof(T),sizeof(T)); return value;
}
void require(bool test,const char* message) {if (!test) throw std::runtime_error(message);}
void writeBmp(const std::filesystem::path& path,const std::vector<uint8_t>& rgba,uint32_t w,uint32_t h) {
  BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+w*h*4;
  BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(w);info.biHeight=-LONG(h);info.biPlanes=1;info.biBitCount=32;
  std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(&file),sizeof(file));f.write(reinterpret_cast<const char*>(&info),sizeof(info));
  for(size_t p=0;p<size_t(w)*h;++p) {
    uint8_t color[4]{};
    for(size_t c=0;c<3;++c) color[2-c]=uint8_t(std::pow(std::clamp(pixel<float>(rgba,p*4+c),0.f,1.f),1/2.2f)*255.f+.5f);
    color[3]=255;f.write(reinterpret_cast<const char*>(color),4);
  }
  if (!f) throw std::runtime_error("Cannot write test image");
}
}
int main(int argc,char** argv) {
  try {
    bool validation=false; std::filesystem::path output;
    for (int i=1;i<argc;++i) {
      std::string arg=argv[i]; if(arg=="--validation") validation=true;
      else if(arg=="--output" && i+1<argc) output=argv[++i];
      else throw std::runtime_error("Usage: rt_test [--validation] [--output existing-directory]");
    }
    {
      Host host;host.init(validation);
      auto mesh=fixture(); Scene scene(host.d,mesh);
      constexpr uint32_t w=96,h=64;
      Frame a(host.d,w,h),b(host.d,w,h),off(host.d,w,h);
      Camera camera;camera.aspect=float(w)/float(h); Camera other=camera;other.origin.y=.25f;
      Camera noShadow=camera;noShadow.flags=Trace;
      Camera disabled=camera;disabled.flags=0;
      host.run([&](auto cmd) {
        scene.recordBuild(cmd); a.initialize(cmd);b.initialize(cmd);off.initialize(cmd);
        scene.recordTrace(cmd,a,camera,other,0);scene.recordTrace(cmd,b,noShadow,other,1);scene.recordTrace(cmd,off,disabled,other,2);
      });
      auto color=host.read(*a.images[0],16),base=host.read(*b.images[0],16),depth=host.read(*a.images[1],4);
      auto ids=host.read(*a.images[3],4),motion=host.read(*a.images[2],8),disabledIds=host.read(*off.images[3],4);
      uint32_t matches=0,shadows=0,foreground=0,shadowMatches=0;
      const auto light=normalize(Vec3{-.7f,-.3f,.4f});
      for (uint32_t y=0;y<h;++y) for (uint32_t x=0;x<w;++x) {
        const size_t p=size_t(y)*w+x;
        Vec3 ray=normalize(Vec3{1,((float(x)+.5f)/w*2-1)*camera.aspect,-((float(y)+.5f)/h*2-1)});
        float distance=camera.farPlane;uint32_t expected=oracle(mesh,camera.origin,ray,distance);
        require(pixel<uint32_t>(ids,p)==expected,"GPU object ID differs from CPU oracle");
        require(pixel<uint32_t>(disabledIds,p)==0,"RT disable still produced a hit");
        if (!expected) continue;
        float expectedDepth=distance*ray.x;
        require(std::abs(pixel<float>(depth,p)-expectedDepth)<.002f,"GPU depth differs from CPU oracle");
        float mv=-.25f/expectedDepth*(float(w)/(2*camera.aspect));
        require(std::abs(pixel<float>(motion,p*2)-mv)<.002f && std::abs(pixel<float>(motion,p*2+1))<.002f,"GPU motion reprojection mismatch");
        float c=pixel<float>(color,p*4),unshadowed=pixel<float>(base,p*4);
        Vec3 hitPoint=mul(ray,distance);hitPoint.x-=.02f;
        float shadowT=camera.farPlane;bool blocked=oracle(mesh,hitPoint,light,shadowT)!=0;
        require(std::isfinite(c) && std::isfinite(unshadowed),"Nonfinite color");
        bool dark=unshadowed-c>.05f;
        require(dark==blocked,"GPU shadow differs from CPU oracle");
        ++shadowMatches;if (dark) ++shadows;if (expected==2) ++foreground;++matches;
      }
      require(matches>1000 && foreground>10 && shadows>10,"Insufficient hit/shadow coverage");
      auto metadata=a.metadata(1,1,camera);
      require(metadata.color.image==encode(a.images[0]->handle) && metadata.width==w,"FGDS image identity mismatch");
      Camera reflective=camera;reflective.flags|=Reflections;
      host.run([&](auto cmd){scene.recordTrace(cmd,off,reflective,other,2);});
      auto reflected=host.read(*off.images[0],16);uint32_t differences=0;
      for(size_t i=0;i<size_t(w)*h*4;++i) {
        require(std::isfinite(pixel<float>(reflected,i)),"Nonfinite reflection");
        if(std::abs(pixel<float>(reflected,i)-pixel<float>(color,i))>.005f) ++differences;
      }
      require(differences>100,"Reflection toggle has no effect");
      if(!output.empty()) {
        writeBmp(output/"rt-shadows.bmp",color,w,h);writeBmp(output/"rt-no-shadows.bmp",base,w,h);
        writeBmp(output/"rt-reflections.bmp",reflected,w,h);
      }
      std::cout << "PASS primary/depth/motion=" << matches << " foreground=" << foreground << " shadow_oracle=" << shadowMatches
        << " shadow_pixels=" << shadows << " reflection_changed_channels=" << differences << " RT_off=no_hits\n";
    }
    require(validationErrors.load()==0,"Vulkan validation errors including resource cleanup");
    return 0;
  } catch(const Unsupported& e) {std::cout << "SKIP: " << e.what() << '\n';return 77;}
    catch(const std::exception& e) {std::cerr << "FAIL: " << e.what() << '\n';return 1;}
}
