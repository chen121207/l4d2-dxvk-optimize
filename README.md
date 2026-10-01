# L4D2 DXVK Optimize

Windows-only research fork of [DXVK](https://github.com/doitsujin/dxvk) for
Left 4 Dead 2. The project keeps the original game, maps, materials and game
logic intact while providing a Vulkan rendering path and an optional external
Vulkan ray-tracing/Frame Generation pipeline.

这是一个面向 Windows 的 DXVK 研究分支，目标是让 Left 4 Dead 2 在保留原有
游戏逻辑、地图、模型和玩法的前提下，通过 Vulkan 渲染，并按需接入独立的
Vulkan 光线追踪和帧生成运行时。

> **Status / 状态:** experimental, offline-only / 实验性、仅限离线测试

The fork is not a new game, a content mod, a D3D12 renderer, or a promise of
VAC compatibility. It is based on the upstream DXVK D3D9 implementation and
adds an opt-in RT bridge for research.

本项目不是重新发布 L4D2，也不是内容 Mod 或 D3D12 渲染器，不能承诺 VAC
兼容。当前核心仍然是 DXVK 的 D3D9 → Vulkan 路线，并在此之上增加可选的
光追桥接实验。

## What this project does / 项目做什么

* **Native DXVK raster path / DXVK 原生栅格路径** — L4D2 remains a 32-bit
  D3D9 application. The packaged `d3d9.dll` translates its calls to Vulkan.
  When RT is disabled, this is the complete normal rendering route.
* **Optional Vulkan RT helper / 可选 Vulkan 光追助手** — supported geometry and
  camera data can be sent from the 32-bit DXVK process to the 64-bit
  `rt_helper.exe`, which owns Vulkan ray-query/acceleration-structure work.
* **FGDS/FFG hand-off / FGDS/FFG 接口** — the helper can produce Color, Depth,
  Motion Vector and Object ID data for the independent
  [FreeFrameGen (FFG)](https://github.com/chen121207/FreeFrameGen) runtime.
  The helper negotiates the frozen FGDS V2 Vulkan ABI and records FFG compute
  on the same Vulkan device as the helper's RT images.
* **Offline launcher / 离线启动器** — the installer provides a launcher that
  selects the L4D2 directory once, stores it per user, and starts a temporary
  `-insecure` session without permanently replacing game DLLs.

## Architecture / 架构

```text
L4D2 (32-bit D3D9)
       |
       v
DXVK d3d9.dll (32-bit, Vulkan raster backend)
       |
       +-- RT off: normal DXVK Vulkan rendering -> Present
       |
       +-- RT on: supported geometry/camera
                   -> shared-memory bridge
                   -> rt_helper.exe (64-bit Vulkan RT)
                   -> RT color/output
                   -> DXVK backbuffer -> Present
                                      |
                                      +-> optional FFG/FGDS input
```

L4D2 is 32-bit while the tested RTX 3050 driver exposes the required ray-query
and acceleration-structure capabilities only to a 64-bit process. The helper
boundary is therefore intentional; it is not a second game executable and it
does not replace the normal DXVK swap chain.

L4D2 是 32 位进程，而当前测试的 RTX 3050 驱动只在 64 位进程中暴露完整的
ray-query 和加速结构能力，因此必须使用 64 位助手进程。助手只负责可选的
光追计算，不替换游戏本身，也不改变普通 DXVK 的 Present 路径。

## Install and run / 安装与运行

1. Run `L4D2-DXVK-RT-Setup-*.exe` and install to a user-writable directory.
2. Start **L4D2 DXVK RT** from the desktop or Start-menu shortcut.
3. On the first run, select the directory containing `left4dead2.exe` and
   `bin\shaderapidx9.dll`. The choice is saved to:
   `%LOCALAPPDATA%\L4D2-DXVK-RT\config.xml`.
4. Choose the rendering options in the launcher:
   * **Enable Vulkan RT / 启用 Vulkan 光追**: enables the opt-in RT bridge.
   * **Enable FreeFrameGen / 启用 FreeFrameGen**: enables the experimental FFG
     hand-off when its DLL is installed and RT is enabled. The launcher disables
     this option while RT is off.
5. Click **Start L4D2 (offline)**. The launcher starts the game with
   `-insecure`, temporarily installs the proxy DLLs for that session, and
   restores the game directory after the game exits.

之后启动会直接读取保存的游戏目录，不会重复询问。运行
`L4D2-DXVK-RT.exe --configure` 可以重新选择目录；`--start` 适合快捷方式或
自动启动；如果上次 Windows/游戏崩溃留下了临时会话，可先使用界面中的
**Recover session / 恢复临时会话**，或者直接给
`tools\Launch-Offline.ps1` 传入 `-Recover`。

The launcher does not overwrite an existing proxy DLL. Close L4D2 before
installing or uninstalling. Uninstall is available from the launcher, Windows
installed-app settings, or `uninstall.exe` in the installation directory.

启动器不会覆盖游戏目录中已有的代理 DLL。安装、卸载和恢复前请先关闭 L4D2。
卸载可以通过启动器、Windows“已安装的应用”，或安装目录中的
`uninstall.exe` 执行。

## RT switch behavior / 光追开关行为

The RT checkbox is an explicit runtime switch, not a claim that every L4D2
draw is ray-traced:

* **Off / 关闭** — `dxvk.enableRayTracing = False`; the game uses the normal
  DXVK Vulkan raster path. No RT helper is required.
* **On / 开启** — `dxvk.enableRayTracing = True`; DXVK attempts to capture only
  supported world geometry and sends it to the x64 helper. If the helper is
  missing, the GPU lacks the required Vulkan features, a frame is invalid, or
  a draw is outside the supported capture set, DXVK keeps the normal raster
  frame instead of presenting invalid geometry.

The setting is stored with the launcher configuration and can be changed at
any time before starting a session. The authoritative package configuration is
`dxvk.conf`; environment variables supplied by the launcher select the helper
and optional FFG runtime.

光追开关只是路径选择开关，并不代表所有材质、粒子和世界绘制都已经完成
光追。关闭时明确走 DXVK 原生 Vulkan 栅格路线；开启时只有满足条件的绘制
才会送入助手，失败时安全回退到栅格帧。

## Native FGDS mode / 原生 FGDS 模式

The word **native** here has a precise scope. FFG's Vulkan ABI records a
compute pass into a caller-owned command buffer and requires the input images,
device and queue family to belong to the same process/device. The current
bridge therefore provides **helper-native FFG**: `rt_helper.exe` owns the
Vulkan images, negotiates capabilities with `ffgVkGetCapabilities`, submits
`FgdsVkPairV2` with the required Color/Depth/Motion/Object-ID flags, and calls
`ffgVkRecordV2` in the same command buffer as the RT pass. The final helper
result is still read back through the existing x86/x64 bridge so DXVK can
present it.

“Game-native” FFG (the L4D2 x86 DXVK process calling the runtime directly) is
not enabled by this release. It needs an x86 FFG Vulkan DLL, DXVK-owned typed
storage images in the four FGDS formats, a command-buffer integration point,
and real raster motion/object-ID resources. Loading the x64 helper DLL from
the game process or passing its Vulkan handles across shared memory would not
meet the FGDS native contract. The standalone bridge smoke test and
`rt_helper.log` identify the negotiated protocol as `protocol=V2`; that proves
helper-native protocol compatibility, not full L4D2 game-native frame
generation.

当前“原生”有明确边界：FFG Vulkan ABI 要求输入图像、VkDevice、队列族和命令
缓冲在同一进程/同一设备中。现版本实现的是**助手进程内原生 FFG**：64 位
`rt_helper.exe` 自己创建 RT 的 Color/Depth/Motion/Object-ID 图像，协商
`ffgVkGetCapabilities`，使用带 CORE 资源位的 `FgdsVkPairV2`，并在同一个
Vulkan command buffer 中调用 `ffgVkRecordV2`。助手最后仍通过已有的 x86/x64
桥接回传结果给 DXVK Present。

这还不是“游戏进程内原生 FFG”。后者需要 Win32 x86 的 FFG Vulkan DLL、DXVK
自己维护的四类 FGDS typed storage image、命令缓冲切入点，以及真实的栅格
Motion/Object-ID 资源。不能把 x64 DLL 加载进 L4D2，或把 helper 的 Vulkan
句柄直接塞进共享内存来冒充原生。smoke 测试日志中的 `protocol=V2` 只证明
助手侧原生协议兼容，不代表 L4D2 已完成游戏内原生插帧。

## Current RT and FFG status / 当前 RT 与 FFG 状态

The standalone RT lab is a real GPU implementation, not a CPU ray-tracing
fallback. It builds BLAS/TLAS, runs a `VK_KHR_ray_query` shader, and verifies
Color/Depth/Motion/Object-ID output in the test host. The x86-to-x64 bridge and
the optional second-frame FFG record path are also covered by development
tests.

The in-game path is still an integration prototype:

* programmable vertex-shader geometry and unsupported post-transform data are
  deliberately skipped to avoid the invalid giant-triangle/grey-frame failure;
* complete L4D2 material, transparency, particle, shadow and world coverage is
  not proven;
* the shared-memory bridge adds synchronization and copy cost, so performance
  can be lower than plain DXVK;
* helper-native FFG now uses the negotiated V2 protocol and correct protocol
  format IDs; it still does not guarantee image quality, latency, or frame
  pacing in every scene;
* DLSS, a production compositor, and a final user-facing RT quality menu are
  not part of this release.

独立 RT 测试程序已经能够在支持的 GPU 上执行真实 Vulkan ray-query，并验证
颜色、深度、运动矢量和 Object ID 输出；但游戏内接入仍是原型。为了避免
快速转动视角时出现错误巨大三角面，当前会跳过无法可靠捕获的可编程顶点
着色器绘制。因此当前版本不能宣称 L4D2 世界已经完整光追，也不能保证
FFG 在所有场景中都稳定或低延迟。

## Safety and online play / 安全与联机

This package is intended for local, offline research only. It always launches
the test session with `-insecure`. Do **not** use the modified renderer or RT
helper on VAC-secured servers, matchmaking, or other protected multiplayer
services. Valve/Steam may treat graphics DLL replacement or process injection
as unsupported even when the code only changes rendering. No VAC-safe claim is
made by this repository.

本项目只用于本地离线研究。启动器会使用 `-insecure`，请不要把修改后的
渲染 DLL、RT 助手或 FFG 运行时带入 VAC 服务器、匹配或其他受保护的联机
服务。仓库不作任何“不会误封”或 VAC 安全承诺。

## Configuration and diagnostics / 配置与诊断

Important files and variables:

| Item | Purpose |
| --- | --- |
| `dxvk.conf` | DXVK options, including `dxvk.enableRayTracing`. |
| `DXVK_CONFIG_FILE` | Selects the package configuration file. |
| `DXVK_CONFIG` | The launcher sets the RT option explicitly for each session. |
| `DXVK_RT_HELPER_PATH` | Overrides the x64 `rt_helper.exe` path. |
| `DXVK_FFG_ENABLE=1` | Requests helper-side FFG when available. |
| `DXVK_FFG_VULKAN_PATH` | Overrides `FreeFrameGenVulkan.dll`. |
| `DXVK_LOG_PATH` | Selects the DXVK log directory. |
| `DXVK_HUD=full` | Shows standard DXVK GPU/FPS/pipeline diagnostics. |
| `%LOCALAPPDATA%\L4D2-DXVK-RT\launcher.log` | Launcher and offline-session log. |
| `rt_helper.log` | x64 helper and FFG bridge diagnostics. |

If RT is enabled but no valid helper output is available, first compare the
same scene with RT disabled. A correct RT-off frame confirms that the ordinary
DXVK path is still being used; it does not prove that RT capture succeeded.

如果打开光追后画面异常，先关闭光追启动同一场景进行对照。关闭时能正常
显示，只能证明 DXVK 栅格路径正常，不能证明游戏内 RT 捕获已经完整。

## Build from source / 从源码构建

This repository retains the upstream DXVK build system and submodules. Clone
with submodules:

```powershell
git clone --recursive https://github.com/chen121207/l4d2-dxvk-optimize.git
cd l4d2-dxvk-optimize
```

Requirements / 环境要求:

* Windows 10/11, Visual Studio C++ tools, and a recent Vulkan driver;
* Meson/Ninja and the Vulkan/SPIR-V headers and shader tools required by DXVK;
* CMake 3.24+ and `glslangValidator` for `experimental/rt`;
* .NET Framework 4.x C# compiler for the self-extracting installer.

Build the x86 DXVK DLL using the normal DXVK Meson configuration. Build the
RT lab separately:

```powershell
cmake -S experimental/rt -B build-rt-x64 -G "Visual Studio 18 2026" -A x64
cmake --build build-rt-x64 --config Release --parallel 4
cmake -S experimental/rt -B build-rt-x86 -G "Visual Studio 18 2026" -A Win32
cmake --build build-rt-x86 --config Release --target rt_bridge_client --parallel 4
```

Run the standalone GPU and bridge checks before packaging:

```powershell
build-rt-x64/Release/rt_test.exe --output artifacts/rt-test
New-Item -ItemType Directory -Force bridge-run | Out-Null
Copy-Item build-rt-x86/Release/rt_bridge_client.exe bridge-run/
Copy-Item build-rt-x64/Release/rt_helper.exe bridge-run/
& .\bridge-run\rt_bridge_client.exe
```

The hardware test may return `77` when the GPU does not expose ray-query,
acceleration-structure, buffer-address and required storage-image features. It
does not silently switch to CPU tracing.

Use `tools/build-installer.ps1` after the x86 DLL, x64 helper and optional FFG
DLL have been built. The script verifies PE architecture, embeds the launcher,
creates the manifest, and produces the self-extracting installer.

## Repository layout / 目录结构

* `src/` — upstream and forked DXVK D3D/Vulkan implementation.
* `experimental/rt/` — x64 RT helper, bridge client, shaders and GPU tests.
* `include/fgds/` — Vulkan FGDS data contract used by the helper/FFG boundary.
* `installer/` — launcher, offline session script, setup/uninstall source and
  application icon.
* `tools/build-installer.ps1` — reproducible Windows installer packaging.
* `RT_FFG_VULKAN.md` — detailed RT/FFG design and current integration notes.

## License and upstream / 许可证与上游

The original DXVK portions retain their upstream zlib/libpng license and
copyright notices. Altered source files are marked in the repository history.
See [LICENSE](LICENSE) before redistribution. This fork is maintained by
`chen121207` and is not an official Valve, NVIDIA, Microsoft, or DXVK release.

原 DXVK 部分保留上游 zlib/libpng 许可证和版权声明；重新分发前请阅读
[LICENSE](LICENSE)。本项目由 `chen121207` 维护，不代表 Valve、NVIDIA、
Microsoft 或 DXVK 官方发布。
