L4D2 DXVK RT — experimental offline package

This build is not proven to render a complete L4D2 world or to be VAC-safe.
It does not permanently install a DLL in the game directory.

Start an offline test from PowerShell:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "tools\Launch-Offline.ps1" -GameDirectory "C:\path\to\Left 4 Dead 2"

Optional frame generation test:
  add -EnableFFG to that command.

The launcher requires the two game d3d9.dll locations to be empty. It never
overwrites an existing proxy. It starts the game with -insecure and removes
its temporary DLLs after the game exits. If the launcher or Windows crashes,
run the same command with -Recover before normal gameplay.

Uninstall through Windows Settings > Installed apps, or run uninstall.exe in
this directory. Uninstall refuses while an offline session record exists;
recover that session first. Modified payload files are retained for safety.
