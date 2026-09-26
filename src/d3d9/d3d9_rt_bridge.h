#pragma once

#include "../../experimental/rt/bridge.hpp"

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace dxvk {

  class D3D9RtBridge {
  public:
    struct Output {
      uint32_t width = 0;
      uint32_t height = 0;
      uint64_t frameId = 0;
      std::vector<uint8_t> color;
      std::vector<float> depth;
      std::vector<float> motion;
      std::vector<uint32_t> objectId;
    };

    D3D9RtBridge();
    ~D3D9RtBridge();

    D3D9RtBridge(const D3D9RtBridge&) = delete;
    D3D9RtBridge& operator=(const D3D9RtBridge&) = delete;

    bool available() const { return m_process != nullptr && m_shared != nullptr; }
    void beginFrame();
    void append(const dxvk::rt::bridge::Triangle& triangle);
    bool render(uint32_t width, uint32_t height,
                const dxvk::rt::bridge::Camera& current,
                const dxvk::rt::bridge::Camera& previous,
                Output& output);

  private:
    HANDLE m_mapping = nullptr;
    HANDLE m_ready = nullptr;
    HANDLE m_done = nullptr;
    HANDLE m_process = nullptr;
    HANDLE m_thread = nullptr;
    uint8_t* m_shared = nullptr;
    std::wstring m_mappingName;
    std::wstring m_readyName;
    std::wstring m_doneName;
    std::vector<dxvk::rt::bridge::Triangle> m_triangles;
    uint64_t m_frameId = 0;
    bool m_loggedUnavailable = false;

    bool start();
    bool send(dxvk::rt::bridge::Command command);
    void stop();
    std::wstring helperPath() const;
    void closeObjects();
  };

}
