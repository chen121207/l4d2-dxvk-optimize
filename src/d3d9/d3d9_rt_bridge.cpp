#include "d3d9_rt_bridge.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace dxvk {

  using namespace dxvk::rt::bridge;

  namespace {
    using namespace dxvk::rt::bridge;

    Control* control(uint8_t* base) { return reinterpret_cast<Control*>(base); }
    Triangle* triangles(uint8_t* base) { return reinterpret_cast<Triangle*>(base + TrianglesOffset); }
    uint8_t* outputPixels(uint8_t* base) { return base + OutputOffset; }
    float* depth(uint8_t* base) { return reinterpret_cast<float*>(base + DepthOffset); }
    float* motion(uint8_t* base) { return reinterpret_cast<float*>(base + MotionOffset); }
    uint32_t* objectId(uint8_t* base) { return reinterpret_cast<uint32_t*>(base + ObjectIdOffset); }


    std::wstring processDirectory() {
      wchar_t path[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, path, MAX_PATH);
      std::wstring result(path);
      const auto slash = result.find_last_of(L"\\/");
      return slash == std::wstring::npos ? std::wstring() : result.substr(0, slash);
    }

    std::wstring environmentPath(const wchar_t* name) {
      wchar_t value[MAX_PATH] = {};
      const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
      return length && length < MAX_PATH ? std::wstring(value, length) : std::wstring();
    }
  }

  D3D9RtBridge::D3D9RtBridge() {
    m_triangles.reserve(32768);
    start();
  }

  D3D9RtBridge::~D3D9RtBridge() {
    stop();
  }

  void D3D9RtBridge::beginFrame() {
    m_triangles.clear();
  }

  void D3D9RtBridge::append(const dxvk::rt::bridge::Triangle& triangle) {
    if (m_triangles.size() < MaxTriangles)
      m_triangles.push_back(triangle);
  }

  std::wstring D3D9RtBridge::helperPath() const {
    const std::wstring configured = environmentPath(L"DXVK_RT_HELPER_PATH");
    if (!configured.empty())
      return configured;
    return processDirectory() + L"\\rt_helper.exe";
  }

  bool D3D9RtBridge::start() {
    if (available())
      return true;

    const std::wstring helper = helperPath();
    if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES)
      return false;

    const DWORD pid = GetCurrentProcessId();
    m_mappingName = name(L"Mapping_", pid);
    m_readyName = name(L"Ready_", pid);
    m_doneName = name(L"Done_", pid);

    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      0, DWORD(SharedSize), m_mappingName.c_str());
    m_ready = CreateEventW(nullptr, FALSE, FALSE, m_readyName.c_str());
    m_done = CreateEventW(nullptr, FALSE, FALSE, m_doneName.c_str());
    if (!m_mapping || !m_ready || !m_done) {
      closeObjects();
      return false;
    }

    m_shared = static_cast<uint8_t*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, SharedSize));
    if (!m_shared) {
      closeObjects();
      return false;
    }

    control(m_shared)->magic = Magic;
    control(m_shared)->version = Version;
    control(m_shared)->fgEnable = environmentPath(L"DXVK_FFG_ENABLE") == L"1" ? 1u : 0u;
    control(m_shared)->fgAlpha = 0.5f;
    InterlockedExchange(&control(m_shared)->command, Idle);
    InterlockedExchange(&control(m_shared)->status, NotReady);

    std::wstring commandLine = L"\"" + helper + L"\" \"" + m_mappingName
      + L"\" \"" + m_readyName + L"\" \"" + m_doneName + L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(0);
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW, nullptr, processDirectory().c_str(), &startup, &process)) {
      closeObjects();
      return false;
    }
    m_process = process.hProcess;
    m_thread = process.hThread;
    return true;
  }

  bool D3D9RtBridge::send(Command command) {
    if (!available())
      return false;
    InterlockedExchange(&control(m_shared)->status, NotReady);
    InterlockedExchange(&control(m_shared)->command, command);
    SetEvent(m_ready);
    HANDLE waits[] = { m_done, m_process };
    const DWORD result = WaitForMultipleObjects(2, waits, FALSE, 30000);
    if (result != WAIT_OBJECT_0)
      return false;
    return InterlockedCompareExchange(&control(m_shared)->status, NotReady, NotReady) == Ok;
  }

  bool D3D9RtBridge::render(uint32_t width, uint32_t height,
                            const dxvk::rt::bridge::Camera& current,
                            const dxvk::rt::bridge::Camera& previous,
                            Output& output) {
    if (!available() || m_triangles.empty() || !width || !height
        || width > MaxWidth || height > MaxHeight)
      return false;

    control(m_shared)->triangleCount = uint32_t(m_triangles.size());
    control(m_shared)->width = width;
    control(m_shared)->height = height;
    control(m_shared)->current = current;
    control(m_shared)->other = previous;
    control(m_shared)->frameId = ++m_frameId;
    std::memcpy(triangles(m_shared), m_triangles.data(), m_triangles.size() * sizeof(Triangle));
    if (!send(UploadScene))
      return false;
    if (!send(Render))
      return false;

    const size_t pixels = size_t(width) * height;
    if (control(m_shared)->outputBytes != pixels * 4
        || control(m_shared)->depthBytes != pixels * sizeof(float)
        || control(m_shared)->motionBytes != pixels * sizeof(float) * 2
        || control(m_shared)->objectIdBytes != pixels * sizeof(uint32_t))
      return false;

    output.width = width;
    output.height = height;
    output.frameId = control(m_shared)->frameId;
    output.color.assign(outputPixels(m_shared), outputPixels(m_shared) + pixels * 4);
    output.depth.assign(depth(m_shared), depth(m_shared) + pixels);
    output.motion.assign(motion(m_shared), motion(m_shared) + pixels * 2);
    output.objectId.assign(objectId(m_shared), objectId(m_shared) + pixels);
    return true;
  }

  void D3D9RtBridge::stop() {
    if (available())
      send(Stop);
    closeObjects();
  }

  void D3D9RtBridge::closeObjects() {
    if (m_shared) {
      UnmapViewOfFile(m_shared);
      m_shared = nullptr;
    }
    if (m_thread) { CloseHandle(m_thread); m_thread = nullptr; }
    if (m_process) { CloseHandle(m_process); m_process = nullptr; }
    if (m_done) { CloseHandle(m_done); m_done = nullptr; }
    if (m_ready) { CloseHandle(m_ready); m_ready = nullptr; }
    if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
  }

}
