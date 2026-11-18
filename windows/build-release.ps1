# Builds a Phomic release for Windows into windows\release\:
#   Phomic-Setup-<version>.exe         installer (Inno Setup 7)
#   Phomic-<version>-portable.zip      the same files, to unpack and run
#
# Needs Visual Studio (C++ / WinUI workload) and Inno Setup 7 (https://jrsoftware.org/isdl.php).
# The version comes from Phomic\Version.h.
#
#   powershell -ExecutionPolicy Bypass -File windows\build-release.ps1

param(
    [string]$Iscc = ""
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# Visual Studio's MSBuild (vswhere also has to be on PATH for the C++ build tools).
$vsInstaller = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer"
$env:PATH = "$vsInstaller;$env:PATH"
$msbuild = & (Join-Path $vsInstaller "vswhere.exe") -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found. Install Visual Studio with the C++ desktop and WinUI workloads." }

if (-not $Iscc) {
    $Iscc = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 7\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Iscc) { throw "Inno Setup 7 not found (ISCC.exe). Install it or pass -Iscc <path>." }

Write-Host "== Building Phomic (Release x64)"
& $msbuild "$root\Phomic.slnx" -t:restore -p:RestorePackagesConfig=true -v:minimal -nologo
if ($LASTEXITCODE) { throw "NuGet restore failed" }
& $msbuild "$root\Phomic.slnx" -p:Configuration=Release -p:Platform=x64 -m -v:minimal -nologo
if ($LASTEXITCODE) { throw "Build failed" }

$app = "$root\x64\Release\Phomic"
$version = (Get-Item "$app\Phomic.exe").VersionInfo.ProductVersion
$out = "$root\release"
New-Item -ItemType Directory -Force $out | Out-Null

Write-Host "== Installer"
& $Iscc /Q "$root\installer\Phomic.iss"
if ($LASTEXITCODE) { throw "Inno Setup failed" }

Write-Host "== Portable zip"
$stage = Join-Path ([System.IO.Path]::GetTempPath()) "Phomic-$version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
Copy-Item $app $stage -Recurse
Get-ChildItem $stage -Recurse -Include *.pdb, *.lib, *.exp, *.ilk | Remove-Item -Force
if (Test-Path "$stage\VBCABLE") { Remove-Item -Recurse -Force "$stage\VBCABLE" } # left over from older builds
New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
Copy-Item "$root\..\LICENSE" "$stage\licenses\Phomic.txt"
Copy-Item "$root\third_party\README.md" "$stage\licenses\third-party.md"
Copy-Item "$root\installer\License.txt" "$stage\licenses\README.txt" # what the installer shows; the zip has no installer
$zip = "$out\Phomic-$version-portable.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item -Recurse -Force $stage

Write-Host "== Done"
Get-ChildItem $out | Where-Object Name -like "*$version*" | ForEach-Object { "{0}  {1:N1} MB" -f $_.Name, ($_.Length / 1MB) }
