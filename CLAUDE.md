# Mario Party DS: Reverse Engineering

Projekt nach dem Skill `re` (`~/.claude/skills/re/SKILL.md`). Stand und Aufgaben stehen in `REVERSE.md`.

- **Binary:** `Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).nds` (nur lesen, nie verändern)
- **Tool-Präfix:** `python tools/` (auf diesem Rechner gibt es `python`, nicht `python3`)
- **Shell:** Windows PowerShell, kein bash, kein jq
- **Sprache:** Notizen auf Deutsch. Die Überschriften aus der Vorlage bleiben englisch, weil Skill und `re_loop.sh` danach suchen.

## Adressen

- Hauptprogramm und ARM7: absolute RAM-Adresse, z. B. `0x02000800`
- Overlays überlappen im RAM, deshalb immer mit Overlay-ID: `ov012:0x021638A0`
- ROM-Positionen als Dateioffset mit Zusatz, z. B. „ROM 0x30DA00“

## labels.csv

Format `addr,name,comment`, Adressen wie oben. Der Kommentar ist optional, aber bei unklaren Adressen erwünscht.

## Plattform

- ARM9-Code ist ARMv5TE, ARM7-Code ARMv4T, beide mit Thumb. Der Modus steht nicht im Binary: `disasm.py` nimmt ARM an, Thumb mit `--thumb`.
- Der Disassembler heißt `tools/disasm.py`, nicht `dis.py` wie im Skill. Ein `dis.py` in `tools/` verdeckt das Python-Standardmodul `dis` und lässt alle Skripte dort abstürzen. Aus demselben Grund keine weiteren Dateien nach Standardmodulen benennen.
- Adressen im Overlay-Bereich brauchen `--ov N`. `xref.py` liefert nur Kandidaten.
- `python tools/test_instruction_set.py` nach jeder Änderung am Decoder ausführen.
- Gerenderte Bilder selbst ansehen: Die Prüfwerte in `gfx_screens.csv` erkennen falsche Paarungen nicht zuverlässig.
- Grafiken in `gfx/` und `web/` sind Spielinhalte und bleiben lokal, nicht veröffentlichen.
- PNG-Ausgabe läuft über `tools/gfx.py` ohne Zusatzpakete; `pypng` wird nicht gebraucht.
- ARM9 und Overlays sind komprimiert. Immer mit `unpacked/arm9.bin` und `unpacked/overlay/ovNNN.bin` arbeiten, nie mit den Bytes aus der ROM. Neu erzeugen mit `python tools/decompress.py <rom>`.
- Emulator: `F:\Tools\melonDS\melonDS.exe`, JIT aus, GDB-Stub an. Der Stub erlaubt **eine Verbindung je melonDS-Start** und muss mit dem Byte `+` beginnen; falsche Bytes blockieren ihn bis zum Neustart. Deshalb immer zuerst einmal `python tools/gdb_server.py` im Hintergrund starten und Werkzeuge nur über `gdb_client.Session` verbinden, nie direkt auf Port 3333.
- Tasten im laufenden Spiel: `python tools/press_keys.py "A,wait=2"`, Bild des melonDS-Fensters: `tools/grab_window.ps1 <png>`. In Menüs erst markieren, dann mit A bestätigen. Nie Spielstände kopieren oder löschen.
- Rekompilierer: `python tools/recomp.py` erzeugt `native/generated/` (übersetzter Spielcode, nie committen). Prüfen: `MarioPartyDS-headless <rom> --stop 0x02003000 --dump run\boot`, dann `tools/check_native_boot.py run\boot`. Ein Reset in melonDS trennt den Stub; für den Vergleich ab Start „Break on startup“ für ARM9 und `gdb_server.py --stay-stopped` verwenden.
- Nativer Lauf bis ins Hauptmenü, Beispiel: `MarioPartyDS-headless <rom> --stop 0xFFFFFFFC --max-blocks 120000000 --name melonDS --save <sav> --input 700:A --input 800:A --input 900:A`. Weitere Optionen: `--dump-tick N` mit `--dump <ordner>`, `--watch ADDR`. Die `.sav` wird nur gelesen.
- Vergleich ab Start: nach frischem melonDS-Start mit „Break on startup“ `python tools/gdb_server.py --stay-stopped`, dann `python tools/compare_ticks_melonds.py <dump-ordner> <letzter tick> <prüfticks...>`. Nach neuen Einsprüngen `tools/recomp_loop.ps1` laufen lassen.
- Nativer Teil: `native/`, bauen mit `powershell -ExecutionPolicy Bypass -File native\build.ps1`. Plan und Entscheidungen stehen in `MULTIPLAYER_ROADMAP.md`.
- `re_loop.sh` läuft hier so noch nicht: bash und jq fehlen.
