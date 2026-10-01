#include "d3d9_device.h"

#include "d3d9_buffer.h"
#include "d3d9_vertex_declaration.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dxvk {

  namespace {
    using dxvk::rt::bridge::Vec3;

    Vec3 xyz(Vector4 value) { return { value.x, value.y, value.z }; }

    bool finite(Vec3 value) {
      return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    Vec3 normalized(Vec3 value) {
      const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
      return length > 1e-6f ? Vec3 { value.x / length, value.y / length, value.z / length } : Vec3 {};
    }

  }

  void D3D9DeviceEx::RtBeginFrame() {
    if (!m_rtBridge)
      return;
    m_rtBridge->beginFrame();
    m_rtHasCamera = false;
    m_rtIncompleteScene = false;
    m_rtObjectId = 1;
  }

  void D3D9DeviceEx::RtEndFrame() {
    RtBeginFrame();
  }

  dxvk::rt::bridge::Camera D3D9DeviceEx::RtGetCamera() const {
    dxvk::rt::bridge::Camera camera;
    const Matrix4& view = m_state.transforms[GetTransformIndex(D3DTS_VIEW)];
    const Matrix4& projection = m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];
    const auto inverseView = tryInverse(view);
    if (!inverseView || std::abs(projection[0].x) < 1e-6f
        || std::abs(projection[1].y) < 1e-6f) {
      camera.flags = 0;
      return camera;
    }

    camera.origin = xyz(*inverseView * Vector4(0, 0, 0, 1));
    camera.right = normalized(xyz(*inverseView * Vector4(1, 0, 0, 0)));
    camera.up = normalized(xyz(*inverseView * Vector4(0, 1, 0, 0)));
    camera.forward = normalized(xyz(*inverseView * Vector4(0, 0, 1, 0)));
    camera.tanHalfFov = 1.0f / std::abs(projection[1].y);
    camera.aspect = std::abs(projection[1].y / projection[0].x);
    camera.farPlane = 10000.0f;
    camera.flags = 1u | 2u;
    if (!finite(camera.origin) || !finite(camera.right) || !finite(camera.up)
        || !finite(camera.forward) || camera.tanHalfFov <= 0 || camera.aspect <= 0)
      camera.flags = 0;
    return camera;
  }

  void D3D9DeviceEx::RtCaptureDraw(D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
                                   const uint8_t* vertices, size_t vertexBytes, UINT stride,
                                   UINT vertexOffset, const uint8_t* indices, size_t indexBytes,
                                   UINT indexStride, UINT startIndex, INT baseVertex) {
    if (!m_rtBridge || !m_rtBridge->available())
      return;
    // Once the bounded bridge scene is full, do not keep walking D3D9 draws
    // that can no longer contribute to the RT frame.
    if (m_rtBridge->triangleCount() >= dxvk::rt::bridge::MaxTriangles)
      return;
    if ((primitiveType != D3DPT_TRIANGLELIST
        && primitiveType != D3DPT_TRIANGLESTRIP
        && primitiveType != D3DPT_TRIANGLEFAN)
        || !m_state.vertexDecl || !vertices || stride < sizeof(float) * 3) {
      return;
    }

    // A programmable D3D9 vertex shader may apply skinning, view/projection,
    // or clip-space post-processing that is not represented by D3DTS_WORLD.
    // Treating its input vertices as world-space geometry creates invalid
    // triangles (most visibly a full-screen gray plane) in the RT BLAS. Until
    // we capture post-VS positions, reject the whole frame instead of replacing
    // a valid raster frame with an incomplete scene.
    if (UseProgrammableVS()) {
      m_rtIncompleteScene = true;
      if (!m_rtLoggedUnsupportedVs) {
        Logger::warn("D3D9 RT: programmable vertex-shader draw is not captureable; keeping the raster frame");
        m_rtLoggedUnsupportedVs = true;
      }
      return;
    }

    // PositionT vertices are already in screen space (HUD, text and many
    // particles). They are not world geometry and applying the world matrix
    // would poison the RT scene with invalid triangles.
    if (m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT))
      return;

    const D3DVERTEXELEMENT9* position = nullptr;
    for (const auto& element : m_state.vertexDecl->GetElements()) {
      if (element.Usage == D3DDECLUSAGE_POSITION && element.UsageIndex == 0
          && (element.Type == D3DDECLTYPE_FLOAT3 || element.Type == D3DDECLTYPE_FLOAT4)) {
        position = &element;
        break;
      }
    }
    const size_t positionBytes = position && position->Type == D3DDECLTYPE_FLOAT4
      ? sizeof(float) * 4 : sizeof(float) * 3;
    if (!position || position->Stream != 0 || position->Offset + positionBytes > stride) {
      return;
    }

    const Matrix4& world = m_state.transforms[GetTransformIndex(D3DTS_WORLD)];
    if (!m_rtHasCamera) {
      m_rtCamera = RtGetCamera();
      m_rtHasCamera = m_rtCamera.flags != 0;
    }
    if (!m_rtHasCamera)
      return;

    auto vertex = [&](uint32_t index, Vec3& result) {
      const int64_t vertexIndex = int64_t(index) + baseVertex;
      if (vertexIndex < 0 || uint64_t(vertexIndex) >= (vertexBytes / stride))
        return false;
      const uint64_t offset = uint64_t(vertexOffset) + uint64_t(vertexIndex) * stride + position->Offset;
      if (offset > vertexBytes || vertexBytes - offset < positionBytes)
        return false;
      float coordinates[4] = { 0, 0, 0, 1 };
      std::memcpy(coordinates, vertices + offset, positionBytes);
      const float w = position->Type == D3DDECLTYPE_FLOAT4 ? coordinates[3] : 1.0f;
      const Vector4 transformed = world * Vector4(coordinates[0], coordinates[1], coordinates[2], w);
      if (!std::isfinite(transformed.w) || std::abs(transformed.w) < 1e-6f)
        return false;
      result = { transformed.x / transformed.w, transformed.y / transformed.w, transformed.z / transformed.w };
      return finite(result);
    };

    auto indexAt = [&](uint64_t element, uint32_t& result) {
      if (!indices) {
        if (element > std::numeric_limits<uint32_t>::max()) return false;
        result = uint32_t(element);
        return true;
      }
      const uint64_t offset = element * indexStride;
      if (offset > indexBytes || indexBytes - offset < indexStride)
        return false;
      result = 0;
      std::memcpy(&result, indices + offset, indexStride);
      return true;
    };

    for (uint32_t primitive = 0; primitive < primitiveCount; ++primitive) {
      if (m_rtBridge->triangleCount() >= dxvk::rt::bridge::MaxTriangles)
        break;
      uint32_t indices3[3];
      Vec3 points[3];
      bool valid = true;
      for (uint32_t corner = 0; corner < 3; ++corner) {
        uint64_t element = 0;
        if (primitiveType == D3DPT_TRIANGLELIST)
          element = uint64_t(startIndex) + uint64_t(primitive) * 3 + corner;
        else if (primitiveType == D3DPT_TRIANGLESTRIP)
          element = uint64_t(startIndex) + uint64_t(primitive) + corner;
        else
          element = uint64_t(startIndex) + (corner == 0 ? 0 : uint64_t(primitive) + corner);
        valid &= indexAt(element, indices3[corner]);
        if (valid) valid &= vertex(indices3[corner], points[corner]);
      }
      if (primitiveType == D3DPT_TRIANGLESTRIP && (primitive & 1)) {
        std::swap(points[1], points[2]);
      }
      if (!valid) {
        break;
      }
      dxvk::rt::bridge::Triangle triangle;
      triangle.a = points[0];
      triangle.b = points[1];
      triangle.c = points[2];
      triangle.id = m_rtObjectId++;
      m_rtBridge->append(triangle);
    }
  }

  void D3D9DeviceEx::RtCapturePrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT count) {
    if (!m_rtBridge || !m_rtBridge->available()) return;
    const auto& stream = m_state.vertexBuffers[0];
    D3D9CommonBuffer* buffer = GetCommonBuffer(stream.vertexBuffer);
    const auto allocation = buffer ? buffer->GetMappedSlice() : nullptr;
    if (!buffer || !allocation || buffer->NeedsReadback()) return;
    RtCaptureDraw(type, count, static_cast<const uint8_t*>(allocation->mapPtr()),
      buffer->Desc()->Size, stream.stride, stream.offset, nullptr, 0, 0, startVertex, 0);
  }

  void D3D9DeviceEx::RtCaptureIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex,
                                                UINT, UINT, UINT startIndex, UINT count) {
    if (!m_rtBridge || !m_rtBridge->available()) return;
    const auto& stream = m_state.vertexBuffers[0];
    D3D9CommonBuffer* vertexBuffer = GetCommonBuffer(stream.vertexBuffer);
    D3D9CommonBuffer* indexBuffer = GetCommonBuffer(m_state.indices);
    const auto vertices = vertexBuffer ? vertexBuffer->GetMappedSlice() : nullptr;
    const auto indices = indexBuffer ? indexBuffer->GetMappedSlice() : nullptr;
    if (!vertexBuffer || !indexBuffer || !vertices || !indices
        || vertexBuffer->NeedsReadback() || indexBuffer->NeedsReadback()) {
      return;
    }
    const auto format = static_cast<D3DFORMAT>(indexBuffer->Desc()->Format);
    const uint32_t indexStride = format == D3DFMT_INDEX16 ? 2 : format == D3DFMT_INDEX32 ? 4 : 0;
    if (!indexStride) return;
    RtCaptureDraw(type, count, static_cast<const uint8_t*>(vertices->mapPtr()),
      vertexBuffer->Desc()->Size, stream.stride, stream.offset,
      static_cast<const uint8_t*>(indices->mapPtr()), indexBuffer->Desc()->Size,
      indexStride, startIndex, baseVertex);
  }

  void D3D9DeviceEx::RtCapturePrimitiveUP(D3DPRIMITIVETYPE type, UINT count,
                                           const void* vertices, UINT stride) {
    if (!m_rtBridge || !m_rtBridge->available()) return;
    const uint64_t vertexCount = GetVertexCount(type, count);
    if (vertexCount > std::numeric_limits<size_t>::max() / stride)
      return;
    const size_t bytes = size_t(vertexCount) * stride;
    RtCaptureDraw(type, count, static_cast<const uint8_t*>(vertices), bytes,
      stride, 0, nullptr, 0, 0, 0, 0);
  }

  void D3D9DeviceEx::RtCaptureIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                  UINT minVertex, UINT numVertices, UINT count,
                                                  const void* indices, D3DFORMAT format,
                                                  const void* vertices, UINT stride) {
    if (!m_rtBridge || !m_rtBridge->available()) return;
    const uint32_t indexStride = format == D3DFMT_INDEX16 ? 2 : format == D3DFMT_INDEX32 ? 4 : 0;
    if (!indexStride) return;
    const uint64_t vertexEnd = uint64_t(minVertex) + uint64_t(numVertices);
    const uint64_t indexCount = GetVertexCount(type, count);
    if (vertexEnd > std::numeric_limits<size_t>::max() / stride
        || indexCount > std::numeric_limits<size_t>::max() / indexStride)
      return;
    RtCaptureDraw(type, count, static_cast<const uint8_t*>(vertices),
      size_t(vertexEnd) * stride, stride, 0,
      static_cast<const uint8_t*>(indices), size_t(indexCount) * indexStride,
      indexStride, 0, 0);
  }

  bool D3D9DeviceEx::RtRenderFrame(uint32_t width, uint32_t height, D3D9RtBridge::Output& output) {
    if (m_rtIncompleteScene) {
      if (!m_rtLoggedIncomplete) {
        Logger::warn("D3D9 RT: frame contains unsupported programmable geometry; RT replacement skipped");
        m_rtLoggedIncomplete = true;
      }
      return false;
    }
    if (!m_rtBridge || !m_rtBridge->available() || !m_rtHasCamera
        || m_rtBridge->sceneOverflowed()
        || m_rtBridge->triangleCount() < dxvk::rt::bridge::MinTrianglesForFrame)
      return false;
    const dxvk::rt::bridge::Camera previous = m_rtHasPreviousCamera ? m_rtPreviousCamera : m_rtCamera;
    const bool success = m_rtBridge->render(width, height, m_rtCamera, previous, output);
    if (success) {
      m_rtPreviousCamera = m_rtCamera;
      m_rtHasPreviousCamera = true;
    }
    return success;
  }

  bool D3D9DeviceEx::RtUploadColor(const Rc<DxvkImage>& image, uint32_t width,
                                   uint32_t height, const std::vector<uint8_t>& color) {
    if (!image || color.size() != size_t(width) * height * 4)
      return false;

    // L4D2's normal swapchain is commonly BGRA8 even though the helper's wire
    // format is RGBA8. Keep the helper API stable and swizzle only at this
    // final copy, otherwise the RT result has reversed red/blue channels.
    const bool bgra = image->info().format == VK_FORMAT_B8G8R8A8_UNORM
                   || image->info().format == VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<uint8_t> uploadColor = color;
    if (bgra) {
      for (size_t i = 0; i < uploadColor.size(); i += 4)
        std::swap(uploadColor[i + 0], uploadColor[i + 2]);
    }
    const VkFormat uploadFormat = bgra
      ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;

    DxvkBufferCreateInfo info;
    info.size = uploadColor.size();
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.stages = VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    info.debugName = "D3D9 RT bridge color upload";
    auto buffer = m_dxvkDevice->createBuffer(info,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
      | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    void* mapped = buffer->mapPtr(0);
    if (!mapped)
      return false;
    std::memcpy(mapped, uploadColor.data(), uploadColor.size());

    // Complete the game's queued draws before injecting the replacement color.
    FlushCsChunk();
    InjectCs([
      cImage = image,
      cBuffer = std::move(buffer),
      cFormat = uploadFormat
    ] (DxvkContext* ctx) mutable {
      ctx->uploadImage(cImage, cBuffer, 0, 4, cFormat);
    });
    return true;
  }

}
