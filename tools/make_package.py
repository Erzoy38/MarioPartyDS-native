"""Puts together the folder and zip for the other players: dist/MarioPartyDS/ and dist/MarioPartyDS.zip.

Usage: python tools/make_package.py

Only the programs, SDL3.dll and texts go in. Never the ROM, a save, the server's key, settings,
known server keys or logs: the script refuses if any such file is among the sources. Build first
with native/build.ps1. dist/ holds translated game code and is not committed.
"""
import hashlib
import os
import shutil
import sys
import zipfile

BUILD = os.path.join("native", "build")
OUT = os.path.join("dist", "MarioPartyDS")
FILES = [
    (os.path.join(BUILD, "MarioPartyDS.exe"), "MarioPartyDS.exe"),
    (os.path.join(BUILD, "SDL3.dll"), "SDL3.dll"),
    (os.path.join(BUILD, "MarioPartyDS-server.exe"), "MarioPartyDS-server.exe"),
    (os.path.join("native", "package", "LIESMICH.txt"), "LIESMICH.txt"),
    ("ONLINE.md", "ONLINE.txt"),
    (os.path.join("native", "third_party", "SDL3", "LICENSE.txt"), "SDL3-LICENSE.txt"),
]
FORBIDDEN = (".nds", ".sav", ".dsv", ".key", ".ini", ".log", ".rejoin")


def main():
    for source, name in FILES:
        if not os.path.exists(source):
            sys.exit(f"missing: {source} (build first)")
        if name.lower().endswith(FORBIDDEN) or source.lower().endswith(FORBIDDEN):
            sys.exit(f"refused: {source} must not be passed on")
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    for source, name in FILES:
        target = os.path.join(OUT, name)
        if name.endswith(".txt"):
            # Notepad reads UTF-8 with Windows line ends best.
            text = open(source, encoding="utf-8").read().replace("\r\n", "\n").replace("\n", "\r\n")
            with open(target, "w", encoding="utf-8-sig", newline="") as f:
                f.write(text)
        else:
            shutil.copy2(source, target)
    archive = os.path.join("dist", "MarioPartyDS.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(OUT)):
            z.write(os.path.join(OUT, name), os.path.join("MarioPartyDS", name))
    for name in sorted(os.listdir(OUT)):
        path = os.path.join(OUT, name)
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]
        print(f"{name:28} {os.path.getsize(path):>11,} bytes  sha256 {digest}")
    print(f"{archive}: {os.path.getsize(archive):,} bytes")


if __name__ == "__main__":
    main()
