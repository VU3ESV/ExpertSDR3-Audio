# Builds a portable zip and an installer from a finished build.
#
#   scripts/package-windows.ps1 -Version 0.2.0 -BuildDir build\Release
#
# Not Authenticode signed: see docs/RELEASING.md.
param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $BuildDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'ExpertSDR3Audio'
$base = "ExpertSDR3Audio-$Version-windows-x64"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Named, not build\Release\*: the test executables are in there too.
Copy-Item (Join-Path $BuildDir 'ExpertSDR3Audio.exe') $stage
Copy-Item LICENSE (Join-Path $stage 'LICENSE.txt')
foreach ($dll in 'fftw3.dll', 'fftw3f.dll', 'deepfilter.dll') {
    $path = Join-Path $BuildDir $dll
    if (-not (Test-Path $path)) { throw "$dll is missing from $BuildDir, so the package would not run" }
    Copy-Item $path $stage
}

windeployqt (Join-Path $stage 'ExpertSDR3Audio.exe') --release --no-translations --no-system-d3d-compiler --no-opengl-sw
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }

# App-local Visual C++ runtime, which Microsoft supports. Without it the app will not start
# on a Windows that has no redistributable installed, which a developer machine never shows.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcDir = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' |
    Sort-Object | Select-Object -Last 1
if (-not $vcDir) { throw 'Could not find the Visual C++ redistributable files' }
foreach ($dll in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll') {
    Copy-Item (Join-Path $vcDir $dll) $stage
}

Write-Host '==> Portable zip'
$zip = Join-Path $dist "$base-portable.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip

Write-Host '==> Installer'
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) {
    choco install innosetup -y --no-progress
    if ($LASTEXITCODE -ne 0) { throw 'Could not install Inno Setup' }
}
& $iscc "/DAppVersion=$Version" "/DStageDir=$stage" "/DOutputDir=$dist" "/DOutputBase=$base-setup" packaging\windows\installer.iss
if ($LASTEXITCODE -ne 0) { throw 'The installer failed to build' }

Get-ChildItem $dist -File | Format-Table Name, Length
