# Builds the runtime, the game code and MarioPartyDS-headless for Android with the NDK's Clang
# (MOBILE_PORT.md, phase 2). The app itself comes with the Gradle project in phase 3.
# Usage: powershell -ExecutionPolicy Bypass -File native\build_android_runtime.ps1 [-Abi arm64-v8a|x86_64]
param([string]$Abi = "arm64-v8a", [string]$Ndk = "F:\Tools\Android\android-ndk-r29")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$cmakeDir = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake"
$env:PATH = "$cmakeDir\CMake\bin;$cmakeDir\Ninja;$env:PATH"
$bin = Join-Path $here "build-android-$Abi"
& cmake -S $here -B $bin -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$Ndk\build\cmake\android.toolchain.cmake" `
    "-DANDROID_ABI=$Abi" -DANDROID_PLATFORM=android-26
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& cmake --build $bin --target MarioPartyDS-headless
if ($LASTEXITCODE -ne 0) { throw "build failed" }
"built: $bin\MarioPartyDS-headless ($((Get-Item "$bin\MarioPartyDS-headless").Length) bytes)"
