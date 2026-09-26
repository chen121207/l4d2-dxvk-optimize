#pragma once

#include "bridge.hpp"

namespace dxvk::rt::bridge {

// The large shared-memory payload is kept out of the public protocol header so
// every D3D9 translation unit does not have to instantiate a 30+ MiB type.
struct Shared {
  Control control{};
  Triangle triangles[MaxTriangles]{};
  uint8_t output[MaxWidth * MaxHeight * 4]{};
  float depth[MaxWidth * MaxHeight]{};
  float motion[MaxWidth * MaxHeight * 2]{};
  uint32_t objectId[MaxWidth * MaxHeight]{};
};

static_assert(offsetof(Shared, triangles) % alignof(Triangle) == 0);
static_assert(offsetof(Shared, triangles) == TrianglesOffset);
static_assert(offsetof(Shared, output) == OutputOffset);
static_assert(offsetof(Shared, depth) == DepthOffset);
static_assert(offsetof(Shared, motion) == MotionOffset);
static_assert(offsetof(Shared, objectId) == ObjectIdOffset);
static_assert(sizeof(Shared) == SharedSize);

} // namespace dxvk::rt::bridge
