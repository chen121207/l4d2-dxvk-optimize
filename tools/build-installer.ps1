[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$DxvkDll,
    [string]$RtHelper,
    [string]$FfgDll
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$project = Split-Path $repo -Parent
$version = '0.1.4'
if (-not $DxvkDll) { $DxvkDll = Join-Path $repo 'build-msvc-x86\src\d3d9\d3d9.dll' }
if (-not $RtHelper) { $RtHelper = Join-Path $repo 'build-rt-x64\Release\rt_helper.exe' }
if (-not $FfgDll) { $FfgDll = Join-Path $project 'FreeFrameGen\build-vulkan\Release\FreeFrameGenVulkan.dll' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'artifacts\installer' }

function Assert-PeMachine([string]$path, [int]$expected) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing build artifact: $path" }
    $stream = [IO.File]::OpenRead($path)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $path" }
        $stream.Position = 0x3C
        $offset = $reader.ReadInt32()
        if ($offset -lt 0x40 -or $offset -gt $stream.Length - 6) { throw "Invalid PE header: $path" }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne $expected) {
            throw "Wrong PE architecture: $path"
        }
    } finally { $stream.Dispose() }
}
Assert-PeMachine $DxvkDll 0x14C
Assert-PeMachine $RtHelper 0x8664
Assert-PeMachine $FfgDll 0x8664
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw '.NET Framework 4.x C# compiler is required' }
$output = Join-Path $OutputDirectory ('L4D2-DXVK-RT-Setup-' + $version + '.exe')
if (Test-Path -LiteralPath $output) { throw "Output exists; choose a fresh -OutputDirectory: $output" }

$work = Join-Path $env:TEMP ('l4d2-dxvk-installer-' + [Guid]::NewGuid().ToString('N'))
$launcherSource = Join-Path $repo 'installer\L4D2DxvkRtLauncher.cs'
$launcherExe = Join-Path $work 'L4D2-DXVK-RT.exe'
[void](New-Item -ItemType Directory -Path $work -Force)
if (-not (Test-Path -LiteralPath $launcherSource -PathType Leaf)) { throw "Missing launcher source: $launcherSource" }
$launcherArguments = @('/nologo','/target:winexe','/platform:x64','/optimize+','/warn:4','/warnaserror+','/codepage:65001',
    '/reference:System.Windows.Forms.dll','/reference:System.Drawing.dll','/reference:System.Xml.dll',
    ('/out:' + $launcherExe), $launcherSource)
& $compiler @launcherArguments
if ($LASTEXITCODE) { throw 'Launcher compilation failed' }
Assert-PeMachine $launcherExe 0x8664
$payload = Join-Path $work 'payload'
[void](New-Item -ItemType Directory -Path (Join-Path $payload 'bin') -Force)
[void](New-Item -ItemType Directory -Path (Join-Path $payload 'tools') -Force)
$payload = (Get-Item -LiteralPath $payload).FullName.TrimEnd('\')
Copy-Item -LiteralPath $DxvkDll -Destination (Join-Path $payload 'bin\d3d9.dll')
Copy-Item -LiteralPath $RtHelper -Destination (Join-Path $payload 'bin\rt_helper.exe')
Copy-Item -LiteralPath $FfgDll -Destination (Join-Path $payload 'bin\FreeFrameGenVulkan.dll')
Copy-Item -LiteralPath (Join-Path $repo 'installer\dxvk.conf') -Destination (Join-Path $payload 'dxvk.conf')
Copy-Item -LiteralPath (Join-Path $repo 'installer\README.txt') -Destination (Join-Path $payload 'README.txt')
Copy-Item -LiteralPath (Join-Path $repo 'installer\Launch-Offline.ps1') -Destination (Join-Path $payload 'tools\Launch-Offline.ps1')
Copy-Item -LiteralPath $launcherExe -Destination (Join-Path $payload 'L4D2-DXVK-RT.exe')

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = Join-Path $work 'payload.zip'
$zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
$entries = @()
function CsString([string]$value) {
    return '"' + $value.Replace('\', '\\').Replace('"', '\"') + '"'
}
try {
    foreach ($file in @(Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object FullName)) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point in payload' }
        $relative = $file.FullName.Substring($payload.Length + 1)
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $file.FullName, $relative.Replace('\','/'), [IO.Compression.CompressionLevel]::Optimal)
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $entries += '        { ' + (CsString $relative) + ', ' + (CsString $hash) + ' },'
    }
} finally { $zip.Dispose() }
if ($entries.Count -ne 7) { throw 'Unexpected payload file count' }
$generated = @"
using System;
using System.Collections.Generic;
internal static class Product {
    internal const string Version = "$version";
    internal static readonly Dictionary<string,string> Manifest =
        new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase) {
$($entries -join [Environment]::NewLine)
        };
}
"@
$source = Join-Path $work 'Product.g.cs'
[IO.File]::WriteAllText($source, $generated, [Text.UTF8Encoding]::new($false))
[void](New-Item -ItemType Directory -Path $OutputDirectory -Force)
$arguments = @('/nologo','/target:winexe','/platform:x64','/optimize+','/warn:4','/warnaserror+','/codepage:65001',
    '/reference:System.Windows.Forms.dll','/reference:System.Drawing.dll',
    '/reference:System.IO.Compression.dll','/reference:System.IO.Compression.FileSystem.dll',
    ('/resource:' + $archive + ',payload.zip'), ('/out:' + $output),
    (Join-Path $repo 'installer\Setup.cs'), $source)
& $compiler @arguments
if ($LASTEXITCODE) { throw 'Installer compilation failed' }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
[IO.File]::WriteAllText($output + '.sha256', $hash + '  ' + [IO.Path]::GetFileName($output) + [Environment]::NewLine)
Write-Output "Installer: $output"
Write-Output "Embedded files: $($entries.Count)"
Write-Output "SHA256: $hash"
Write-Output 'Unsigned experimental offline build; no permanent game DLL installation.'
