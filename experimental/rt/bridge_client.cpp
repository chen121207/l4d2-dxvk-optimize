#include "bridge.hpp"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dxvk::rt;
using namespace dxvk::rt::bridge;

namespace {
struct Handle { HANDLE value = nullptr; ~Handle() { if (value) CloseHandle(value); } };
void fixture(std::vector<Triangle>& triangles) {
  auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint32_t id) {
    triangles.push_back({a, id, b, 0, c}); triangles.push_back({a, id, c, 0, d});
  };
  quad({10, -30, -30}, {10, 30, -30}, {10, 30, 30}, {10, -30, 30}, 1);
  quad({5, -1, -1}, {5, 1, -1}, {5, 1, 1}, {5, -1, 1}, 2);
}
void command(Shared* shared, HANDLE ready, HANDLE done, Command value) {
  InterlockedExchange(&shared->control.status, NotReady);
  InterlockedExchange(&shared->control.command, value);
  SetEvent(ready);
  if (WaitForSingleObject(done, 30000) != WAIT_OBJECT_0) throw std::runtime_error("bridge response timeout");
  if (InterlockedCompareExchange(&shared->control.status, NotReady, NotReady) != Ok)
    throw std::runtime_error("bridge helper rejected command");
}
}

int wmain() {
  try {
    const DWORD pid = GetCurrentProcessId();
    const auto mappingName = name(L"Mapping_", pid), readyName = name(L"Ready_", pid), doneName = name(L"Done_", pid);
    Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(sizeof(Shared)), mappingName.c_str())};
    Handle ready{CreateEventW(nullptr, FALSE, FALSE, readyName.c_str())};
    Handle done{CreateEventW(nullptr, FALSE, FALSE, doneName.c_str())};
    if (!mapping.value || !ready.value || !done.value) throw std::runtime_error("bridge object creation failed");
    auto* shared = static_cast<Shared*>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) throw std::runtime_error("bridge mapping failed");
    struct View { Shared* value; ~View() { if (value) UnmapViewOfFile(value); } } view{shared};
    // A file mapping is zero-filled; C++ default member initializers do not run
    // across process boundaries, so initialize the wire header explicitly.
    shared->control.magic = Magic;
    shared->control.version = Version;
    InterlockedExchange(&shared->control.command, Idle);
    InterlockedExchange(&shared->control.status, NotReady);
    const auto module = [] {
      wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH); std::wstring result(path);
      return result.substr(0, result.find_last_of(L"\\/"));
    }();
    std::wstring commandLine = L"\"" + module + L"\\rt_helper.exe\" \"" + mappingName + L"\" \"" + readyName + L"\" \"" + doneName + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end()); mutableCommand.push_back(0);
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, module.c_str(), &startup, &process))
      throw std::runtime_error("cannot start 64-bit RT helper");
    Handle processHandle{process.hProcess}, threadHandle{process.hThread};
    std::vector<Triangle> triangles; fixture(triangles);
    shared->control.triangleCount = uint32_t(triangles.size()); std::copy(triangles.begin(), triangles.end(), shared->triangles);
    command(shared, ready.value, done.value, UploadScene);
    shared->control.width = 96; shared->control.height = 64;
    shared->control.current = Camera{}; shared->control.current.aspect = 96.0f / 64.0f;
    shared->control.other = shared->control.current; shared->control.other.origin.y = .25f;
    command(shared, ready.value, done.value, Render);
    if (shared->control.outputBytes != 96u * 64u * 4u) throw std::runtime_error("invalid RT output size");
    size_t lit = 0; for (size_t i = 0; i < shared->control.outputBytes; i += 4) if (shared->output[i] || shared->output[i+1] || shared->output[i+2]) ++lit;
    if (lit < 1000) throw std::runtime_error("RT helper returned an empty image");
    command(shared, ready.value, done.value, Stop);
    WaitForSingleObject(processHandle.value, 30000);
    DWORD helperExit = STILL_ACTIVE;
    GetExitCodeProcess(processHandle.value, &helperExit);
    if (helperExit != 0) throw std::runtime_error("RT helper exited with failure");
    std::wcout << L"PASS cross-bitness RT bridge pixels=" << lit << L" helper_exit=" << helperExit << L"\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
