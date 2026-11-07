"""Puts together public/: the online parts that hold no game code, as a repository of their own.

Usage: python tools/make_public.py

Copies the server, the lobby client side, the encryption and the test tools from this repository,
plus the files that exist only for the public one (native/public/). public/ has its own git history
and is ignored here; committing and pushing there is done by hand. Refuses if a copied file holds
something that points at the game: memory addresses, overlay names, ROM or save files.
"""
import os
import re
import shutil
import subprocess
import sys

OUT = "public"
FILES = [
    (os.path.join("native", "server", "server.cpp"), os.path.join("server", "server.cpp")),
    (os.path.join("native", "runtime", "lobby_protocol.h"), os.path.join("src", "lobby_protocol.h")),
    (os.path.join("native", "runtime", "lobby.h"), os.path.join("src", "lobby.h")),
    (os.path.join("native", "runtime", "lobby.cpp"), os.path.join("src", "lobby.cpp")),
    (os.path.join("native", "runtime", "secure.h"), os.path.join("src", "secure.h")),
    (os.path.join("native", "runtime", "secure.cpp"), os.path.join("src", "secure.cpp")),
    (os.path.join("tools", "lobby_rules.py"), os.path.join("tools", "lobby_rules.py")),
    (os.path.join("tools", "netsim.py"), os.path.join("tools", "netsim.py")),
    (os.path.join("native", "public", "lobby_client.cpp"), os.path.join("client", "lobby_client.cpp")),
    (os.path.join("native", "public", "CMakeLists.txt"), "CMakeLists.txt"),
    (os.path.join("native", "public", "build.ps1"), "build.ps1"),
    (os.path.join("native", "public", "README.md"), "README.md"),
    (os.path.join("native", "public", "gitignore.txt"), ".gitignore"),
    ("LICENSE", "LICENSE"),
]
# Put at the top of every source file in public/.
NOTICE = "Copyright (c) 2026 Erzoy38 (https://github.com/Erzoy38). Alle Rechte vorbehalten. Siehe LICENSE."
COMMENT = {".cpp": "// ", ".h": "// ", ".py": "# ", ".ps1": "# "}
# Game addresses (main RAM, overlays), the unpacked program, ROM and save files.
FORBIDDEN = re.compile(r"0x0[0-9A-Fa-f]{7}|\bov\d{3}\b|overlay|arm9|arm7|unpacked|\.nds\b|\.sav\b|recomp|"
                       r"generated/|melonDS|headless", re.I)
# Text in the copied files that says where they came from, rewritten for the public layout.
REWRITES = {
    os.path.join("src", "lobby_protocol.h"): [
        ("// The lobby protocol between MarioPartyDS(.exe / -headless) and MarioPartyDS-server.",
         "// The lobby protocol between the game's client and MarioPartyDS-server."),
        ("Messages are framed like the game's network\n// messages in net.cpp:",
         "Messages are framed like the game's own network\n// messages:"),
        ("//   guest -> server: the game's own frames (net.cpp), passed",
         "//   guest -> server: the game's own frames, passed"),
    ],
    os.path.join("tools", "lobby_rules.py"): [
        ('SERVER = os.path.join("native", "build", "MarioPartyDS-server.exe")',
         'SERVER = os.path.join("build", "MarioPartyDS-server.exe")'),
        ("client in the encryption tests below and in netsim_test.py.",
         "client in the encryption tests below."),
        ('parser.add_argument("--client", nargs="+", required=True)',
         'parser.add_argument("--client", nargs="+", default=[os.path.join("build", "lobby-client.exe")])'),
    ],
}


def main():
    problems = []
    texts = {}
    for source, target in FILES:
        text = open(source, encoding="utf-8").read().replace("\r\n", "\n")
        for old, new in REWRITES.get(target, []):
            if old not in text:
                sys.exit(f"rewrite not found in {source}: {old!r}")
            text = text.replace(old, new)
        prefix = COMMENT.get(os.path.splitext(target)[1])
        if prefix:
            text = prefix + NOTICE + "\n" + text
        for number, line in enumerate(text.split("\n"), start=1):
            if FORBIDDEN.search(line):
                problems.append(f"{source}:{number}: {line.strip()}")
        texts[target] = text
    if problems:
        print("refused, these lines point at the game:")
        print("\n".join(problems))
        return 1
    os.makedirs(OUT, exist_ok=True)
    for entry in os.listdir(OUT):
        if entry in (".git", "build", "run"):
            continue
        path = os.path.join(OUT, entry)
        shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
    for target, text in texts.items():
        path = os.path.join(OUT, target)
        os.makedirs(os.path.dirname(path) or OUT, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    if not os.path.isdir(os.path.join(OUT, ".git")):
        subprocess.run(["git", "init", "-q", "-b", "main", OUT], check=True)
    for target in sorted(texts):
        print(target)
    print(f"{len(texts)} files in {OUT}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
