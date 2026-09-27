[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GameDirectory,
    [switch]$EnableFFG,
    [switch]$Recover,
    [string]$Map = 'c2m2_fairgrounds'
)
$ErrorActionPreference = 'Stop'
$package = Split-Path $PSScriptRoot -Parent
$game = (Resolve-Path -LiteralPath $GameDirectory).Path.TrimEnd('\')
$gameExe = Join-Path $game 'left4dead2.exe'
$shader = Join-Path $game 'bin\shaderapidx9.dll'
$dll = Join-Path $package 'bin\d3d9.dll'
$helper = Join-Path $package 'bin\rt_helper.exe'
$ffg = Join-Path $package 'bin\FreeFrameGenVulkan.dll'
$config = Join-Path $package 'dxvk.conf'
$state = Join-Path $package 'offline-session.json'
$targets = @((Join-Path $game 'd3d9.dll'), (Join-Path $game 'bin\d3d9.dll'))

function Assert-NoReparse([string]$path) {
    for ($cursor = $path; $cursor; $cursor = Split-Path $cursor -Parent) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point refused: $cursor"
            }
        }
        if ((Split-Path $cursor -Parent) -eq $cursor) { break }
    }
}
function Assert-GameClosed {
    if (Get-Process -Name left4dead2 -ErrorAction SilentlyContinue) {
        throw 'Close L4D2 before changing the temporary DXVK DLLs.'
    }
}
function Recover-Session {
    if (-not (Test-Path -LiteralPath $state -PathType Leaf)) { return }
    Assert-GameClosed
    Assert-NoReparse $state
    $saved = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
    if ($saved.product -ne 'L4D2-DXVK-RT-Offline' -or $saved.version -ne 1 -or
        $saved.game -ne $game -or $saved.sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or
        $saved.targets.Count -ne 2 -or $saved.targets[0] -ne $targets[0] -or
        $saved.targets[1] -ne $targets[1]) {
        throw 'Offline session record does not match this game. No file was removed.'
    }
    foreach ($target in $targets) {
        Assert-NoReparse $target
        if (Test-Path -LiteralPath $target) {
            if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
                (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $saved.sha256) {
                throw "Changed temporary DLL preserved: $target"
            }
        }
    }
    foreach ($target in $targets) {
        if (Test-Path -LiteralPath $target -PathType Leaf) { Remove-Item -LiteralPath $target }
    }
    Remove-Item -LiteralPath $state
    Write-Output 'Temporary DXVK DLLs removed; original game files were not changed.'
}

if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf) -or
    -not (Test-Path -LiteralPath $shader -PathType Leaf)) {
    throw 'This is not a valid L4D2 game directory.'
}
foreach ($path in @($game, $shader, $dll, $helper, $ffg, $config)) { Assert-NoReparse $path }
foreach ($path in @($dll, $helper, $ffg, $config)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing installed component: $path" }
}
if ($Recover) { Recover-Session; return }
Assert-GameClosed
if (Test-Path -LiteralPath $state) {
    throw "An earlier session needs recovery. Re-run with -Recover: $state"
}
foreach ($target in $targets) {
    Assert-NoReparse $target
    if (Test-Path -LiteralPath $target) { throw "Existing D3D9 proxy preserved: $target" }
}
if ($Map -notmatch '^[A-Za-z0-9_]+$') { throw 'Invalid map name.' }

$hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
# Write the ownership record before the first game-directory copy. A crash can
# then be recovered explicitly without touching unknown pre-existing files.
@{ product = 'L4D2-DXVK-RT-Offline'; version = 1; game = $game;
   sha256 = $hash; targets = $targets; createdUtc = [DateTime]::UtcNow.ToString('o') } |
    ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $state -Encoding UTF8
$variables = @('DXVK_CONFIG_FILE','DXVK_RT_HELPER_PATH','DXVK_FFG_ENABLE','DXVK_FFG_VULKAN_PATH','DXVK_LOG_PATH')
$old = @{}
foreach ($name in $variables) { $old[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($target in $targets) {
        Copy-Item -LiteralPath $dll -Destination $target -ErrorAction Stop
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hash) {
            throw "Temporary DLL copy verification failed: $target"
        }
    }
    $env:DXVK_CONFIG_FILE = $config
    $env:DXVK_RT_HELPER_PATH = $helper
    $env:DXVK_FFG_ENABLE = if ($EnableFFG) { '1' } else { '0' }
    $env:DXVK_FFG_VULKAN_PATH = $ffg
    $env:DXVK_LOG_PATH = $package
    $arguments = @('-steam','-insecure','-novid','-windowed','-w','1280','-h','720','+sv_lan','1')
    if ($Map) { $arguments += @('+map', $Map) }
    Write-Output 'Starting L4D2 offline with -insecure. Do not enter VAC servers.'
    $process = Start-Process -FilePath $gameExe -WorkingDirectory $game -ArgumentList $arguments -PassThru
    $process.WaitForExit()
    while (Get-Process -Name left4dead2 -ErrorAction SilentlyContinue) {
        Start-Sleep -Seconds 1
    }
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $old[$name], 'Process') }
    Recover-Session
}
