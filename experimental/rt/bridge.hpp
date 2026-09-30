#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dxvk::rt::bridge {
constexpr uint32_t Magic = 0x3254524Cu; // L4RT
constexpr uint32_t Version = 4;
constexpr uint32_t MaxTriangles = 250000;
// A few triangles can come from menus, particles, or debug overlays. Do not
// replace a complete raster frame with a sparse RT scene unless a reasonable
// amount of world geometry was captured.
constexpr uint32_t MinTrianglesForFrame = 128;
constexpr uint32_t MaxWidth = 1920;
constexpr uint32_t MaxHeight = 1080;
// Keep the wire protocol independent of the Vulkan implementation headers.
// These PODs are intentionally layout-compatible with dxvk::rt::{Vec3,
// Triangle,Camera}, but are small enough to include from the 32-bit D3D9 DLL.
struct Vec3 { float x, y, z; };
struct Triangle {
  Vec3 a; uint32_t id;
  Vec3 b; uint32_t pad0 = 0;
  Vec3 c; uint32_t pad1 = 0;
  Vec3 albedo{0.6f, 0.6f, 0.6f}; uint32_t pad2 = 0;
};
static_assert(sizeof(Triangle) == 64);
struct Camera {
  Vec3 origin{0, 0, 0}; float tanHalfFov = 1;
  Vec3 forward{1, 0, 0}; float aspect = 1;
  Vec3 right{0, 1, 0}; uint32_t flags = 1 | 2;
  Vec3 up{0, 0, 1}; float farPlane = 10000;
};
static_assert(sizeof(Camera) == 64);
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
  uint32_t depthBytes = 0;
  uint32_t motionBytes = 0;
  uint32_t objectIdBytes = 0;
  uint64_t frameId = 0;
  uint32_t fgEnable = 0;
  // The D3D9 side only needs color for the present path. Auxiliary images are
  // opt-in until a native FGDS consumer asks for a CPU readback.
  uint32_t auxEnable = 0;
  float fgAlpha = 0.5f;
  Camera current{};
  Camera other{};
};
constexpr size_t alignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
constexpr size_t TrianglesOffset = alignUp(sizeof(Control), alignof(Triangle));
constexpr size_t OutputOffset = TrianglesOffset + size_t(MaxTriangles) * sizeof(Triangle);
constexpr size_t DepthOffset = OutputOffset + size_t(MaxWidth) * MaxHeight * 4;
constexpr size_t MotionOffset = DepthOffset + size_t(MaxWidth) * MaxHeight * sizeof(float);
constexpr size_t ObjectIdOffset = MotionOffset + size_t(MaxWidth) * MaxHeight * 2 * sizeof(float);
constexpr size_t SharedSize = ObjectIdOffset + size_t(MaxWidth) * MaxHeight * sizeof(uint32_t);
static_assert(sizeof(Control) < 512);

inline std::wstring name(const wchar_t* prefix, DWORD pid) {
  return std::wstring(L"Local\\L4D2RT_") + prefix + std::to_wstring(pid);
}
} // namespace dxvk::rt::bridge
