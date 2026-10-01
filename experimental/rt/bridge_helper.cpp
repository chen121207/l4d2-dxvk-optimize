#include "bridge_layout.hpp"
#include "test_host.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

using namespace dxvk::rt;
using namespace dxvk::rt::bridge;
using namespace dxvk::rt::testing;

namespace {
struct Handle { HANDLE value = nullptr; ~Handle() { if (value) CloseHandle(value); } };
struct FfgVkContext;
using FfgVkCreate = VkResult (__cdecl*)(VkPhysicalDevice, VkDevice, uint32_t,
  PFN_vkGetDeviceProcAddr, FfgVkContext**);
using FfgVkDestroy = void (__cdecl*)(FfgVkContext*);
using FfgVkGetCapabilities = VkResult (__cdecl*)(FfgVkContext*, FgdsVkCapabilities*);
using FfgVkRecord = VkResult (__cdecl*)(FfgVkContext*, VkCommandBuffer,
  const FgdsVkPair*, const FgdsVkImage*);
using FfgVkRecordV2 = VkResult (__cdecl*)(FfgVkContext*, VkCommandBuffer,
  const FgdsVkPairV2*, const FgdsVkImage*);

struct FfgRuntime {
  HMODULE module = nullptr;
  FfgVkContext* context = nullptr;
  FfgVkDestroy destroy = nullptr;
  FfgVkGetCapabilities getCapabilities = nullptr;
  FfgVkRecord record = nullptr;
  FfgVkRecordV2 recordV2 = nullptr;
  FgdsVkCapabilities capabilities{};
  bool useV2 = false;

  ~FfgRuntime() {
    if (context && destroy) destroy(context);
    if (module) FreeLibrary(module);
  }

  bool init(Host& host, std::ofstream& log) {
    wchar_t configured[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"DXVK_FFG_VULKAN_PATH", configured, MAX_PATH);
    std::wstring path;
    if (length && length < MAX_PATH) {
      path.assign(configured, length);
    } else {
      wchar_t exe[MAX_PATH]{};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      path.assign(exe);
      path.resize(path.find_last_of(L"\\/"));
      path += L"\\FreeFrameGenVulkan.dll";
    }
    module = LoadLibraryW(path.c_str());
    if (!module) {
      log << "FFG Vulkan runtime not found: " << std::filesystem::path(path).string() << '\n';
      return false;
    }
    auto create = reinterpret_cast<FfgVkCreate>(GetProcAddress(module, "ffgVkCreate"));
    destroy = reinterpret_cast<FfgVkDestroy>(GetProcAddress(module, "ffgVkDestroy"));
    getCapabilities = reinterpret_cast<FfgVkGetCapabilities>(
      GetProcAddress(module, "ffgVkGetCapabilities"));
    record = reinterpret_cast<FfgVkRecord>(GetProcAddress(module, "ffgVkRecord"));
    recordV2 = reinterpret_cast<FfgVkRecordV2>(GetProcAddress(module, "ffgVkRecordV2"));
    if (!create || !destroy || !record || create(host.d.physical, host.d.handle, host.d.family,
      host.getDevice, &context) != VK_SUCCESS) {
      log << "FFG Vulkan runtime exported ABI is unavailable\n";
      if (module) { FreeLibrary(module); module = nullptr; }
      context = nullptr; destroy = nullptr; getCapabilities = nullptr;
      record = nullptr; recordV2 = nullptr;
      return false;
    }
    if (getCapabilities && recordV2) {
      capabilities = {};
      capabilities.structSize = sizeof(capabilities);
      if (getCapabilities(context, &capabilities) == VK_SUCCESS
          && capabilities.version >= FGDS_VK_VERSION_0_2
          && (capabilities.requiredResources & FGDS_VK_RESOURCE_CORE)
             == FGDS_VK_RESOURCE_CORE
          && capabilities.colorFormat == FGDS_VK_FORMAT_RGBA32_FLOAT
          && capabilities.depthFormat == FGDS_VK_FORMAT_R32_FLOAT
          && capabilities.motionFormat == FGDS_VK_FORMAT_RG32_FLOAT
          && capabilities.objectIdFormat == FGDS_VK_FORMAT_R32_UINT) {
        useV2 = true;
        log << "FFG Vulkan runtime initialized (FGDS V2 native, requiredResources=0x"
          << std::hex << capabilities.requiredResources << std::dec << ")\n";
      } else {
        log << "FFG V2 capability negotiation failed; using frozen V1 ABI\n";
      }
    } else {
      log << "FFG Vulkan runtime initialized (frozen V1 ABI)\n";
    }
    return true;
  }

  VkResult recordPair(VkCommandBuffer commandBuffer, const FgdsVkPair& pair,
                      const FgdsVkPairV2& pairV2, const FgdsVkImage& output) const {
    if (useV2)
      return recordV2(context, commandBuffer, &pairV2, &output);
    return record(context, commandBuffer, &pair, &output);
  }
};
dxvk::rt::Triangle runtimeTriangle(const dxvk::rt::bridge::Triangle& source) {
  dxvk::rt::Triangle result;
  result.a = { source.a.x, source.a.y, source.a.z };
  result.b = { source.b.x, source.b.y, source.b.z };
  result.c = { source.c.x, source.c.y, source.c.z };
  result.id = source.id;
  result.albedo = { source.albedo.x, source.albedo.y, source.albedo.z };
  return result;
}
dxvk::rt::Camera runtimeCamera(const dxvk::rt::bridge::Camera& source) {
  dxvk::rt::Camera result;
  result.origin = { source.origin.x, source.origin.y, source.origin.z };
  result.tanHalfFov = source.tanHalfFov;
  result.forward = { source.forward.x, source.forward.y, source.forward.z };
  result.aspect = source.aspect;
  result.right = { source.right.x, source.right.y, source.right.z };
  result.flags = source.flags;
  result.up = { source.up.x, source.up.y, source.up.z };
  result.farPlane = source.farPlane;
  return result;
}
std::ofstream diagnostic() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring file(path);
  file.resize(file.find_last_of(L"\\/"));
  file += L"\\rt_helper.log";
  return std::ofstream(std::filesystem::path(file), std::ios::app);
}
void setStatus(Shared* shared, Status status) {
  InterlockedExchange(&shared->control.status, status);
}
void writeOutput(Shared* shared, const std::vector<uint8_t>& floats, uint32_t width, uint32_t height) {
  const size_t pixels = size_t(width) * height;
  if (pixels > size_t(MaxWidth) * MaxHeight || floats.size() < pixels * 16)
    throw std::runtime_error("RT helper output exceeds bridge buffer");
  for (size_t p = 0; p < pixels; ++p) {
    auto channel = [&](size_t c) {
      float value = 0;
      std::memcpy(&value, floats.data() + (p * 4 + c) * sizeof(float), sizeof(value));
      value = std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
      return uint8_t(std::pow(value, 1.0f / 2.2f) * 255.0f + 0.5f);
    };
    shared->output[p * 4 + 0] = channel(0);
    shared->output[p * 4 + 1] = channel(1);
    shared->output[p * 4 + 2] = channel(2);
    shared->output[p * 4 + 3] = 255;
  }
  shared->control.outputBytes = uint32_t(pixels * 4);
}
void writeAuxiliary(Shared* shared, const std::vector<uint8_t>& depth,
                    const std::vector<uint8_t>& motion, const std::vector<uint8_t>& ids,
                    uint32_t width, uint32_t height) {
  const size_t pixels = size_t(width) * height;
  if (pixels > size_t(MaxWidth) * MaxHeight || depth.size() != pixels * sizeof(float)
      || motion.size() != pixels * sizeof(float) * 2 || ids.size() != pixels * sizeof(uint32_t))
    throw std::runtime_error("RT helper auxiliary output size mismatch");
  std::memcpy(shared->depth, depth.data(), depth.size());
  std::memcpy(shared->motion, motion.data(), motion.size());
  std::memcpy(shared->objectId, ids.data(), ids.size());
  shared->control.depthBytes = uint32_t(depth.size());
  shared->control.motionBytes = uint32_t(motion.size());
  shared->control.objectIdBytes = uint32_t(ids.size());
}
}

int wmain(int argc, wchar_t** argv) {
  auto log = diagnostic();
  if (argc != 4) {
    log << "invalid argument count\n";
    std::wcerr << L"Usage: rt_helper.exe <mapping> <ready-event> <done-event>\n";
    return 2;
  }
  try {
    Handle mapping{OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, argv[1])};
    Handle ready{OpenEventW(SYNCHRONIZE, FALSE, argv[2])};
    Handle done{OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3])};
    if (!mapping.value || !ready.value || !done.value)
      throw Unsupported("bridge objects cannot be opened");
    auto* shared = static_cast<Shared*>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) throw std::runtime_error("bridge mapping cannot be mapped");
    struct View { Shared* value; ~View() { if (value) UnmapViewOfFile(value); } } view{shared};
    if (shared->control.magic != Magic || shared->control.version != Version)
      throw std::runtime_error("bridge protocol mismatch");
    Host host; host.init(false);
    log << "host initialized\n";
    std::unique_ptr<Scene> scene;
    std::unique_ptr<Frame> frame;
    std::unique_ptr<Frame> previousFrame;
    std::unique_ptr<Image> ffgOutput;
    FfgRuntime ffg;
    bool ffgAvailable = false;
    bool frameImagesInitialized = false;
    bool sceneInitialized = false;
    bool havePrevious = false;
    bool ffgOutputInitialized = false;
    if (shared->control.fgEnable)
      ffgAvailable = ffg.init(host, log);
    else
      log << "FFG disabled by bridge control\n";
    for (;;) {
      if (WaitForSingleObject(ready.value, 30000) != WAIT_OBJECT_0)
        throw std::runtime_error("bridge command timeout");
      const auto command = static_cast<Command>(InterlockedCompareExchange(&shared->control.command, Idle, Idle));
      try {
        if (command == Stop) {
          setStatus(shared, Ok); SetEvent(done.value); return 0;
        } else if (command == UploadScene) {
          const auto count = shared->control.triangleCount;
          if (!count || count > MaxTriangles) throw std::runtime_error("invalid scene triangle count");
          std::vector<dxvk::rt::Triangle> triangles;
          triangles.reserve(count);
          for (uint32_t i = 0; i < count; i++)
            triangles.push_back(runtimeTriangle(shared->triangles[i]));
          scene = std::make_unique<Scene>(host.d, triangles);
          sceneInitialized = false; setStatus(shared, Ok);
        } else if (command == Render) {
          if (!scene) throw std::runtime_error("scene was not uploaded");
          const auto width = shared->control.width, height = shared->control.height;
          if (!width || !height || width > MaxWidth || height > MaxHeight)
            throw std::runtime_error("invalid bridge extent");
          if (!frame || frame->images[0]->width != width || frame->images[0]->height != height) {
            frame = std::make_unique<Frame>(host.d, width, height);
            previousFrame = std::make_unique<Frame>(host.d, width, height);
            ffgOutput = std::make_unique<Image>(host.d, width, height, VK_FORMAT_R32G32B32A32_SFLOAT);
            frameImagesInitialized = false;
            sceneInitialized = false;
            havePrevious = false;
            ffgOutputInitialized = false;
          }
          bool generated = false;
          host.run([&](VkCommandBuffer commandBuffer) {
            if (!frame->images[0]->handle) throw std::runtime_error("invalid RT frame");
            if (!frame->images[0]->device) throw std::runtime_error("frame has no device");
            if (!frame->images[0]->width) throw std::runtime_error("invalid RT frame width");
            // The first frame initializes new images; subsequent frames are already GENERAL.
            if (!frameImagesInitialized) {
              frame->initialize(commandBuffer);
              previousFrame->initialize(commandBuffer);
              frameImagesInitialized = true;
            }
            if (!sceneInitialized) {
              scene->recordBuild(commandBuffer);
              sceneInitialized = true;
            }
            const auto previousCamera = runtimeCamera(shared->control.other);
            const auto currentCamera = runtimeCamera(shared->control.current);
            scene->recordTrace(commandBuffer, *frame, currentCamera, previousCamera, 0);
            // The RT kernel writes motion from the frame camera to the
            // reference camera.  Once a second endpoint exists, refresh the
            // previous endpoint with the reverse direction as well.  FGDS
            // requires pair[0] old->new and pair[1] new->old; using the
            // previous frame's old->older vector here would violate that
            // contract even though ffgVkRecord could validate the handles.
            if (shared->control.fgEnable && ffgAvailable && havePrevious)
              scene->recordTrace(commandBuffer, *previousFrame,
                previousCamera, currentCamera, 1);
            if (shared->control.fgEnable && ffgAvailable && havePrevious) {
              if (!ffgOutputInitialized) {
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.image = ffgOutput->handle;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                host.d.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                ffgOutputInitialized = true;
              }
              FgdsVkPair pair{};
              pair.structSize = sizeof(pair);
              pair.version = FGDS_VK_VERSION;
              pair.frames[0] = previousFrame->metadata(shared->control.frameId - 1,
                (shared->control.frameId - 1) * 16666666ull, previousCamera);
              pair.frames[1] = frame->metadata(shared->control.frameId,
                shared->control.frameId * 16666666ull, currentCamera);
              pair.alpha = std::clamp(shared->control.fgAlpha, 0.0f, 1.0f);
              FgdsVkPairV2 pairV2{};
              pairV2.structSize = sizeof(pairV2);
              pairV2.version = FGDS_VK_VERSION_0_2;
              pairV2.frames[0] = previousFrame->metadataV2(shared->control.frameId - 1,
                (shared->control.frameId - 1) * 16666666ull, previousCamera);
              pairV2.frames[1] = frame->metadataV2(shared->control.frameId,
                shared->control.frameId * 16666666ull, currentCamera);
              pairV2.alpha = pair.alpha;
              const auto outputImage = ffgOutput->fgds();
              if (ffg.recordPair(commandBuffer, pair, pairV2, outputImage) != VK_SUCCESS)
                throw std::runtime_error("FFG Vulkan record failed");
              log << "FFG frame generated id=" << shared->control.frameId
                << (ffg.useV2 ? " protocol=V2" : " protocol=V1") << '\n';
              generated = true;
            }
          });
          if (generated)
            writeOutput(shared, host.read(*ffgOutput, 16), width, height);
          else
            writeOutput(shared, host.read(*frame->images[0], 16), width, height);
          if (shared->control.auxEnable) {
            writeAuxiliary(shared, host.read(*frame->images[1], 4), host.read(*frame->images[2], 8),
              host.read(*frame->images[3], 4), width, height);
          } else {
            shared->control.depthBytes = 0;
            shared->control.motionBytes = 0;
            shared->control.objectIdBytes = 0;
          }
          std::swap(frame, previousFrame);
          havePrevious = true;
          setStatus(shared, Ok);
        } else {
          throw std::runtime_error("unknown bridge command");
        }
      } catch (const std::exception& error) {
        std::cerr << "bridge command failed: " << error.what() << '\n';
        log << "bridge command failed: " << error.what() << '\n';
        shared->control.outputBytes = 0;
        shared->control.depthBytes = 0;
        shared->control.motionBytes = 0;
        shared->control.objectIdBytes = 0;
        setStatus(shared, Failed);
      }
      InterlockedExchange(&shared->control.command, Idle);
      SetEvent(done.value);
    }
  } catch (const Unsupported& error) {
    std::cerr << "SKIP: " << error.what() << '\n';
    log << "SKIP: " << error.what() << '\n';
    return 77;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    log << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
