#include "bridge.hpp"
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
    bool initialized = false;
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
          std::vector<Triangle> triangles(shared->triangles, shared->triangles + count);
          scene = std::make_unique<Scene>(host.d, triangles);
          frame.reset(); initialized = false; setStatus(shared, Ok);
        } else if (command == Render) {
          if (!scene) throw std::runtime_error("scene was not uploaded");
          const auto width = shared->control.width, height = shared->control.height;
          if (!width || !height || width > MaxWidth || height > MaxHeight)
            throw std::runtime_error("invalid bridge extent");
          if (!frame || frame->images[0]->width != width || frame->images[0]->height != height) {
            frame = std::make_unique<Frame>(host.d, width, height);
            initialized = false;
          }
          host.run([&](VkCommandBuffer commandBuffer) {
            if (!frame->images[0]->handle) throw std::runtime_error("invalid RT frame");
            if (!frame->images[0]->device) throw std::runtime_error("frame has no device");
            if (!frame->images[0]->width) throw std::runtime_error("invalid RT frame width");
            // The first frame initializes new images; subsequent frames are already GENERAL.
            if (!initialized) { frame->initialize(commandBuffer); scene->recordBuild(commandBuffer); initialized = true; }
            scene->recordTrace(commandBuffer, *frame, shared->control.current, shared->control.other, 0);
          });
          writeOutput(shared, host.read(*frame->images[0], 16), width, height);
          setStatus(shared, Ok);
        } else {
          throw std::runtime_error("unknown bridge command");
        }
      } catch (const std::exception& error) {
        std::cerr << "bridge command failed: " << error.what() << '\n';
        log << "bridge command failed: " << error.what() << '\n';
        shared->control.outputBytes = 0; setStatus(shared, Failed);
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
