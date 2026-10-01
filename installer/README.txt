L4D2 DXVK RT — experimental offline package

This build is not proven to render a complete L4D2 world or to be VAC-safe.
It does not permanently install a DLL in the game directory.

The RT bridge currently refuses programmable vertex-shader geometry until
post-vertex-shader capture is implemented. This prevents invalid full-screen
triangles from replacing the raster frame; a successful helper start alone is
not proof that L4D2 world lighting is ray-traced.

安装完成后，开始菜单和桌面会创建“L4D2 DXVK RT”快捷方式。
也可以直接运行 L4D2-DXVK-RT.exe：首次运行会让你选择 L4D2 游戏目录，
并保存到 %LOCALAPPDATA%\L4D2-DXVK-RT\config.xml。之后启动会直接读取该配置，
不再重复选择。需要更换游戏目录时运行：
  L4D2-DXVK-RT.exe --configure

启动器会调用离线脚本；也可以从 PowerShell 手动启动：
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "tools\Launch-Offline.ps1" -GameDirectory "C:\path\to\Left 4 Dead 2"

Optional frame generation test:
  add -EnableFFG to that command.

Manual RT test:
  add -EnableRayTracing (or the compatibility alias -EnableRT) to that command.

The launcher also provides an "Enable DXVK ray tracing" switch. It is off by
default. With the switch off, the session writes a temporary configuration
with `dxvk.enableRayTracing = False` and uses the normal DXVK Vulkan raster
route; the RT helper and FFG path are not started. With it on, the packaged
64-bit RT helper is used as an experimental path. FFG is only enabled when
both RT and the FFG checkbox are enabled.

启动器还提供“启用 DXVK 光线追踪”开关，默认关闭。关闭时会创建临时配置并明确
写入 `dxvk.enableRayTracing = False`，使用原生 DXVK Vulkan 栅格路径，不启动
RT 助手和 FFG；开启时才会使用 64 位 RT 助手实验路径。只有同时开启光追和
FFG 选项时，才会启用 FFG。

The launcher requires the two game d3d9.dll locations to be empty. It never
overwrites an existing proxy. It starts the game with -insecure and removes
its temporary DLLs after the game exits. If the launcher or Windows crashes,
run the same command with -Recover before normal gameplay.

可以在启动器中点击“卸载”，也可以通过 Windows 设置 → 已安装的应用，
或运行安装目录中的 uninstall.exe 卸载。卸载拒绝在离线会话仍存在时执行；
请先恢复该会话。被修改过的 payload 文件会为安全起见保留。
