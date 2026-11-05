# Builds native/ with the Visual Studio Build Tools and runs the smoke test.
# Usage: powershell -ExecutionPolicy Bypass -File native\build.ps1
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmakeDir = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake"
$tools = "$cmakeDir\CMake\bin;$cmakeDir\Ninja"
$src = $here
$bin = Join-Path $here "build"

$script = @"
@echo off
call "$vcvars" >nul
set "PATH=$tools;%PATH%"
cmake -S "$src" -B "$bin" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "$bin" || exit /b 1
"@
$cmd = Join-Path $env:TEMP "mpds_build_$PID.cmd"
Set-Content -Path $cmd -Value $script -Encoding ascii
try {
    & cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed with $LASTEXITCODE" }
} finally {
    Remove-Item $cmd -Force
}

$exe = Join-Path $bin "MarioPartyDS.exe"
$run = Start-Process -FilePath $exe -ArgumentList "--smoke-test" -PassThru
if (-not $run.WaitForExit(10000)) {
    $run.Kill()
    throw "smoke test did not finish within 10 s"
}
if ($run.ExitCode -ne 0) { throw "smoke test exit code $($run.ExitCode)" }
"smoke test passed: $exe ($((Get-Item $exe).Length) bytes)"
