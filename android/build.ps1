# Builds the Android app: a signed APK (to install directly) and an AAB (App Bundle).
# Usage: powershell -ExecutionPolicy Bypass -File android\build.ps1 [-Tools F:\Tools\Android]
#
# Needs in $Tools: jdk-17*, sdk (platform 35, build-tools 35.0.0, cmake 3.31.6), android-ndk-r29.
# Needs in android/: local.properties (sdk.dir, mpds.ndkPath) and keystore.properties (release key);
# both stay local. Without keystore.properties the release builds come out unsigned.
# Needs native/generated (python tools/recomp.py) and native/third_party (SDL3-src, mbedtls, stb, fonts).
param([string]$Tools = "F:\Tools\Android")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$jdk = Get-ChildItem $Tools -Directory -Filter "jdk-17*" | Select-Object -First 1
if (-not $jdk) { throw "no JDK 17 in $Tools" }
$env:JAVA_HOME = $jdk.FullName
$env:GRADLE_USER_HOME = Join-Path $Tools "gradle-home"
$env:ANDROID_USER_HOME = Join-Path $Tools "user"
# Temporary files (bundletool, CMake) next to the tools, not on a possibly full system drive.
$temp = Join-Path $Tools "tmp"
New-Item -ItemType Directory -Force $temp | Out-Null
$env:TMP = $temp
$env:TEMP = $temp
Push-Location $here
try {
    & .\gradlew.bat --stop | Out-Null  # a daemon started before would keep the old TEMP
    & .\gradlew.bat assembleRelease bundleRelease --console=plain
    if ($LASTEXITCODE -ne 0) { throw "gradle failed with $LASTEXITCODE" }
} finally {
    Pop-Location
}
Get-ChildItem "$here\app\build\outputs\apk\release\*.apk", "$here\app\build\outputs\bundle\release\*.aab" |
    ForEach-Object { "{0,8:N1} MB  {1}" -f ($_.Length / 1MB), $_.FullName }
