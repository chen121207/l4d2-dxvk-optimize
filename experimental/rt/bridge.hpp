#pragma once
#include "rt.hpp"
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace dxvk::rt::bridge {
constexpr uint32_t Magic = 0x3254524Cu; // L4RT
constexpr uint32_t Version = 1;
constexpr uint32_t MaxTriangles = 250000;
constexpr uint32_t MaxWidth = 1280;
constexpr uint32_t MaxHeight = 720;
enum Command : uint32_t { Idle = 0, UploadScene = 1, Render = 2, Stop = 3 };
enum Status : int32_t { NotReady = -1, Ok = 0, BadRequest = 1, Failed = 2 };
struct Control {
  uint32_t magic = Magic;
  uint32_t version = Version;
  volatile LONG command = Idle;
  volatile LONG status = NotReady;
  uint32_t triangleCount = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t outputBytes = 0;
  Camera current{};
  Camera other{};
};
struct Shared {
  Control control{};
  Triangle triangles[MaxTriangles]{};
  uint8_t output[MaxWidth * MaxHeight * 4]{};
};
static_assert(offsetof(Shared, triangles) % alignof(Triangle) == 0);
static_assert(sizeof(Control) < 512);

inline std::wstring name(const wchar_t* prefix, DWORD pid) {
  return std::wstring(L"Local\\L4D2RT_") + prefix + std::to_wstring(pid);
}
} // namespace dxvk::rt::bridge
