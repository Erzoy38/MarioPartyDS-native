# Sets up a second Windows PC (e.g. a laptop) to build the Android app from this repository.
#
# Before: install Git for Windows, then clone the repository and run this from its folder:
#   git clone https://github.com/Erzoy38/MarioPartyDS-native.git
#   cd MarioPartyDS-native
#   powershell -ExecutionPolicy Bypass -File tools\setup_laptop.ps1
#
# The repository is public; what must stay private (translated game code, the release key) lies in
# the releases of the private repository Erzoy38/MarioPartyDS-private, which needs the owner's login.
#
# What it does (each step is skipped when already done, so it can simply be run again):
#   1. GitHub CLI into $Tools\gh; you log in once in the browser (gh auth login --web).
#   2. The project data that is not in Git (private release "data": native/third_party, native/assets,
#      native/generated, analysis tables) into this folder.
#   3. JDK 17, Android command-line tools, SDK packages (sdkmanager asks you to accept the licences),
#      NDK r29, all into $Tools. Downloads are checked against the SHA-256 of the files used on the PC.
#   4. The release key from the same private release; android/keystore.properties asks for its two
#      passwords (they are not on GitHub) and stays on this PC only.
#   5. Builds APK and AAB with android\build.ps1 (skip with -NoBuild).
# Not included on purpose: the ROM, savegames, game graphics (gfx/, web/), unpacked/.
# The Windows program needs Visual Studio Build Tools in addition; not set up here.
param(
    [string]$Tools = "$env:USERPROFILE\MPDS-Tools",
    [switch]$NoBuild
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # Invoke-WebRequest is many times faster without the bar
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$vault = "Erzoy38/MarioPartyDS-private"  # release "data": mpds-laptop-data.zip and mpds-release.jks
New-Item -ItemType Directory -Force $Tools, "$Tools\dl", "$Tools\keys" | Out-Null

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }

function Get-Checked($url, $file, $sha256) {
    if (-not (Test-Path $file) -or (Get-FileHash $file).Hash -ne $sha256) {
        Write-Host "downloading $url"
        Invoke-WebRequest $url -OutFile $file -UseBasicParsing
    }
    $hash = (Get-FileHash $file).Hash
    if ($hash -ne $sha256) { throw "$file has SHA-256 $hash, expected $sha256" }
}

function Expand-Into($zip, $target) {
    New-Item -ItemType Directory -Force $target | Out-Null
    & tar.exe -x -f $zip -C $target
    if ($LASTEXITCODE -ne 0) { throw "unpacking $zip failed" }
}

Step "1. GitHub CLI"
$gh = "$Tools\gh\bin\gh.exe"
if (-not (Test-Path $gh)) {
    $zip = "$Tools\dl\gh_2.103.3_windows_amd64.zip"
    Get-Checked "https://github.com/cli/cli/releases/download/v2.103.3/gh_2.103.3_windows_amd64.zip" $zip `
        "98D0CFA794721665D7973DD728C78AFF7BD836D58F10F6D7A03568ECCB0A22E0"
    Expand-Into $zip "$Tools\gh"
}
cmd /c "`"$gh`" auth status >nul 2>&1"  # PowerShell 5.1 would turn redirected stderr into errors
if ($LASTEXITCODE -ne 0) {
    Write-Host "Log in to GitHub: answer 'n' to 'Authenticate Git', then enter the code in the browser."
    & $gh auth login --hostname github.com --git-protocol https --web
    if ($LASTEXITCODE -ne 0) { throw "GitHub login failed" }
}

Step "2. Project data (private release data)"
if (-not (Test-Path "$root\native\generated") -or -not (Test-Path "$root\native\third_party\SDL3-src")) {
    $data = "$Tools\dl\mpds-laptop-data.zip"
    if (-not (Test-Path $data)) {
        & $gh release download data --repo $vault --pattern mpds-laptop-data.zip --dir "$Tools\dl"
        if ($LASTEXITCODE -ne 0) { throw "download of the project data failed" }
    }
    Expand-Into $data $root
} else {
    Write-Host "already there"
}

Step "3. JDK 17, Android SDK and NDK"
$jdk = "$Tools\jdk-17.0.21+9"
if (-not (Test-Path "$jdk\bin\java.exe")) {
    $zip = "$Tools\dl\jdk17.zip"
    Get-Checked "https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.21%2B9/OpenJDK17U-jdk_x64_windows_hotspot_17.0.21_9.zip" `
        $zip "98518ECF554465057EE5509462631DEEEE374BC368C12A411D225CD29381F84B"
    Expand-Into $zip $Tools
}
$env:JAVA_HOME = $jdk
$sdk = "$Tools\sdk"
$sdkmanager = "$sdk\cmdline-tools\latest\bin\sdkmanager.bat"
if (-not (Test-Path $sdkmanager)) {
    $zip = "$Tools\dl\commandlinetools-win.zip"
    Get-Checked "https://dl.google.com/android/repository/commandlinetools-win-16111833_latest.zip" $zip `
        "E5885E2E59038C0778A85FC19F01DE7CE7C567C105553F7617B936B1E0E87B2B"
    Expand-Into $zip "$Tools\dl\cmdline"
    New-Item -ItemType Directory -Force "$sdk\cmdline-tools" | Out-Null
    Move-Item "$Tools\dl\cmdline\cmdline-tools" "$sdk\cmdline-tools\latest"
}
$packages = @("platform-tools", "platforms;android-35", "build-tools;35.0.0", "build-tools;34.0.0", "cmake;3.31.6")
if (-not (Test-Path "$sdk\platforms\android-35") -or -not (Test-Path "$sdk\cmake\3.31.6") -or -not (Test-Path "$sdk\platform-tools")) {
    Write-Host "The SDK licences follow: read them and answer y to accept."
    & $sdkmanager "--sdk_root=$sdk" --licenses
    & $sdkmanager "--sdk_root=$sdk" @packages
    if ($LASTEXITCODE -ne 0) { throw "sdkmanager failed" }
}
$ndk = "$Tools\android-ndk-r29"
if (-not (Test-Path "$ndk\ndk-build.cmd")) {
    $zip = "$Tools\dl\android-ndk-r29-windows.zip"
    Get-Checked "https://dl.google.com/android/repository/android-ndk-r29-windows.zip" $zip `
        "4F83A1A87EA0D33AE2B43812CE27B768BE949BC78ACF90B955134D19E3068F1C"
    Expand-Into $zip $Tools
}
$slash = { param($p) ($p -replace '\\', '/') -replace ':', '\:' }
$local = "sdk.dir=$(& $slash $sdk)`nmpds.ndkPath=$(& $slash $ndk)`n"
[IO.File]::WriteAllText("$root\android\local.properties", $local)

Step "4. Release key"
$key = "$Tools\keys\mpds-release.jks"
$keyHash = "EA135FB5DADC21BF5F61BA826C8AF5EBB59C0AF5D7CC03C15C74DA8466B611F2"
if (-not (Test-Path $key) -or (Get-FileHash $key).Hash -ne $keyHash) {
    & $gh release download data --repo $vault --pattern mpds-release.jks --dir "$Tools\keys" --clobber
    if ($LASTEXITCODE -ne 0) { throw "download of the release key failed" }
    if ((Get-FileHash $key).Hash -ne $keyHash) { throw "the downloaded key does not match" }
}
$props = "$root\android\keystore.properties"
if (-not (Test-Path $props)) {
    Write-Host "The key's passwords are not on GitHub. Type them now; they go to android\keystore.properties only."
    $store = Read-Host "storePassword"
    $keyPass = Read-Host "keyPassword (Enter: same as storePassword)"
    if ([string]::IsNullOrEmpty($keyPass)) { $keyPass = $store }
    $text = "storeFile=$(& $slash $key)`nstorePassword=$store`nkeyAlias=mpds`nkeyPassword=$keyPass`n"
    [IO.File]::WriteAllText($props, $text)
}

Step "5. Build"
if ($NoBuild) {
    Write-Host "skipped (-NoBuild). Later: powershell -ExecutionPolicy Bypass -File android\build.ps1 -Tools `"$Tools`""
} else {
    & powershell -ExecutionPolicy Bypass -File "$root\android\build.ps1" -Tools $Tools
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
Write-Host "`nDone. adb for a connected phone: $sdk\platform-tools\adb.exe" -ForegroundColor Green
