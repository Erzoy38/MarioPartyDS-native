#!/bin/bash
# Builds the iOS app on a Mac with Xcode. See MOBILE_PORT.md, phase 6.
#
#   MPDS_IOS_TEAM=ABCDE12345 ios/build.sh            an .ipa for your registered iPhones and iPads
#   ios/build.sh --simulator                          the app for the iOS Simulator (no Apple account needed)
#   ios/build.sh --unsigned                           an unsigned .ipa for iPhones and iPads; a tool such as
#                                                     Sideloadly signs it with your Apple ID when installing
#
# Needs: Xcode with the iOS SDK, CMake 3.25 or newer, and native/generated/ (translated game code,
# made from the ROM by tools/recomp.py; copy the folder over from Windows or run the tool here).
# native/third_party/ must hold SDL3-src, mbedtls-3.6.4, stb and fonts, as for Android.
# The output stays under native/build-ios/ (not committed). The .ipa holds translated game code:
# keep it private, like the Windows program.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
native="$root/native"
out="$native/build-ios"

simulator=0
unsigned=0
if [ "${1:-}" = "--simulator" ]; then
    simulator=1
    out="$native/build-ios-sim"
elif [ "${1:-}" = "--unsigned" ]; then
    unsigned=1
    out="$native/build-ios-unsigned"
fi

if ! ls "$native"/generated/*.cpp >/dev/null 2>&1; then
    echo "native/generated is empty: run python3 tools/recomp.py first (or copy it from Windows)" >&2
    exit 1
fi
for dir in SDL3-src mbedtls-3.6.4 stb fonts; do
    if [ ! -d "$native/third_party/$dir" ]; then
        echo "native/third_party/$dir is missing" >&2
        exit 1
    fi
done

team="${MPDS_IOS_TEAM:-}"
if [ $simulator = 0 ] && [ $unsigned = 0 ] && [ -z "$team" ]; then
    echo "Set MPDS_IOS_TEAM to your Apple developer team ID (Xcode > Settings > Accounts)," >&2
    echo "or build for the simulator with --simulator." >&2
    exit 1
fi

sysroot=iphoneos
[ $simulator = 1 ] && sysroot=iphonesimulator

cmake -G Xcode -S "$native" -B "$out" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=$sysroot \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DCMAKE_XCODE_GENERATE_SCHEME=ON \
    -DMPDS_IOS_TEAM="$team"

project="$out/MarioPartyDS.xcodeproj"

if [ $simulator = 1 ]; then
    # Apple silicon Macs; the simulator needs no signing.
    xcodebuild -project "$project" -scheme MarioPartyDS -configuration Release \
        -sdk iphonesimulator -destination "generic/platform=iOS Simulator" \
        CODE_SIGNING_ALLOWED=NO build
    app="$out/Release-iphonesimulator/MarioPartyDS.app"
    [ -d "$app" ] || app="$(find "$out" -maxdepth 4 -type d -name MarioPartyDS.app -path '*iphonesimulator*' | head -n 1)"
    echo
    echo "Built: $app"
    echo "Into a running simulator:  xcrun simctl install booted \"$app\""
    echo "Start it:                  xcrun simctl launch booted io.github.erzoy38.mariopartyds"
    exit 0
fi

if [ $unsigned = 1 ]; then
    # For devices, without signing; an .ipa is the .app in a folder Payload/, zipped.
    xcodebuild -project "$project" -scheme MarioPartyDS -configuration Release \
        -sdk iphoneos -destination "generic/platform=iOS" \
        CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build
    app="$out/Release-iphoneos/MarioPartyDS.app"
    [ -d "$app" ] || app="$(find "$out" -maxdepth 4 -type d -name MarioPartyDS.app -path '*iphoneos*' | head -n 1)"
    [ -d "$app" ] || { echo "MarioPartyDS.app not found under $out" >&2; exit 1; }
    rm -rf "$out/ipa"
    mkdir -p "$out/ipa/Payload"
    cp -R "$app" "$out/ipa/Payload/"
    (cd "$out/ipa" && zip -qry MarioPartyDS-unsigned.ipa Payload)
    echo
    echo "Built: $out/ipa/MarioPartyDS-unsigned.ipa"
    echo "Install it with Sideloadly (Windows or Mac), which signs it with your Apple ID."
    exit 0
fi

archive="$out/MarioPartyDS.xcarchive"
xcodebuild -project "$project" -scheme MarioPartyDS -configuration Release \
    -destination "generic/platform=iOS" -archivePath "$archive" \
    -allowProvisioningUpdates archive

options="$out/ExportOptions.plist"
sed "s/TEAM_ID/$team/" "$here/ExportOptions.plist" > "$options"
xcodebuild -exportArchive -archivePath "$archive" -exportPath "$out/ipa" \
    -exportOptionsPlist "$options" -allowProvisioningUpdates

echo
echo "Built: $(ls "$out"/ipa/*.ipa)"
echo "Onto a connected device: Xcode > Window > Devices and Simulators, or Apple Configurator."
