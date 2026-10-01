L4D2 DXVK RT — experimental offline package

This build is not proven to render a complete L4D2 world or to be VAC-safe.
It does not permanently install a DLL in the game directory.

安装完成后，开始菜单和桌面会创建“L4D2 DXVK RT”快捷方式。
也可以直接运行 L4D2-DXVK-RT.exe：首次运行会让你选择 L4D2 游戏目录，
并保存到 %LOCALAPPDATA%\L4D2-DXVK-RT\config.xml。之后启动会直接读取该配置，
不再重复选择。需要更换游戏目录时运行：
  L4D2-DXVK-RT.exe --configure

启动器会调用离线脚本；也可以从 PowerShell 手动启动：
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "tools\Launch-Offline.ps1" -GameDirectory "C:\path\to\Left 4 Dead 2"

Optional frame generation test:
  add -EnableFFG to that command.

The launcher requires the two game d3d9.dll locations to be empty. It never
overwrites an existing proxy. It starts the game with -insecure and removes
its temporary DLLs after the game exits. If the launcher or Windows crashes,
run the same command with -Recover before normal gameplay.

可以在启动器中点击“卸载”，也可以通过 Windows 设置 → 已安装的应用，
或运行安装目录中的 uninstall.exe 卸载。卸载拒绝在离线会话仍存在时执行；
请先恢复该会话。被修改过的 payload 文件会为安全起见保留。
