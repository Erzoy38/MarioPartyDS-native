# Mario Party DS (Nintendo DS, Europa, Rev 1) — Reverse Engineering Notes

## Binary Identification

| Field | Value |
|-------|-------|
| File | `Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).nds` |
| Format | NDS-Cartridge-Image (NitroROM) |
| Platform | Nintendo DS |
| CPU | ARM946E-S (ARM9, ARMv5TE) und ARM7TDMI (ARM7, ARMv4T) |
| ROM/File size | 0x2000000 (32 MiB), davon laut Header 0x1F5F725 belegt |
| Header size | 0x4000 |
| Entry point | ARM9 0x02000800, ARM7 0x02380000 |
| Titel / Gamecode / Maker | `MARIOPARTYDS` / `A8TP` / `01` |
| ROM-Version | 1 |
| Header-CRC / Logo-CRC | 0x7566 / 0xCF56 (beide nachgerechnet, stimmen) |
| SDK-Version | 0x04007531 (4.0, Build 30001) |

---

## Memory Map

*(Adressen aus dieser ROM; DS-Hardwarebereiche werden ergänzt, sobald der Code sie nutzt)*

| Address Range | Purpose |
|---------------|---------|
| 0x02000000-0x020BDC80 | ARM9-Hauptprogramm, entpackt (static) |
| 0x020BDC80-0x021638A0 | ARM9 static BSS |
| 0x020BDC80-0x020C2F40 | Autoload-Daten im entpackten Image (0x52C0 Byte, werden beim Start kopiert) |
| 0x020C2F40-0x020C2F58 | Autoload-Liste, 2 Einträge |
| 0x01FF8000-0x01FFD220 | ITCM, Autoload-Ziel 1, 0x5220 Byte. CP15 c9,c1,1 = 0x20: Basis 0, 32 MiB virtuell (Spiegelungen), also liegt die Adresse darin |
| 0x027E0000-0x027E02C0 | DTCM, Autoload-Ziel 2, 0xA0 Byte plus 0x220 BSS. CP15 c9,c1,0 = 0x027E000A: Basis 0x027E0000, 16 KiB. Die Stacks liegen ab 0x027E3FC0 abwärts |
| 0x021638A0-0x021A4FE0 | Overlay-Bereich inklusive BSS (74 Overlays laden nach 0x021638A0, 34 nach 0x021838A0) |
| 0x02380000-... | ARM7-Programm (Größe in der ROM 0x27258) |

Hinweis: `autoload_start` und `static_bss_start` sind beide 0x020BDC80. Die Autoload-Daten liegen im Image also dort, wo zur Laufzeit das BSS beginnt.

---

## Data-Range Map

| Start | End | Size | Classification | Notes |
|-------|-----|------|----------------|-------|
| 0x0 | 0x4000 | 0x4000 | header | CRCs geprüft |
| 0x4000 | 0x76F88 | 0x72F88 | code, BLZ-komprimiert | ARM9; siehe Kompression |
| 0x76F88 | 0x77000 | 0x78 | unclassified | Lücke, vermutlich Padding |
| 0x77000 | 0x77D80 | 0xD80 | table | ARM9-Overlay-Tabelle, 108 Einträge |
| 0x77E00 | 0x2E0C88 | 0x268E88 | code | Overlays 0-107, 107 davon als komprimiert markiert |
| 0x2E0E00 | 0x308058 | 0x27258 | code | ARM7 |
| 0x308200 | 0x30B679 | 0x3479 | table | FNT |
| 0x30B800 | 0x30CE10 | 0x1610 | table | FAT, 706 Einträge |
| 0x30D000 | 0x30DA00 | 0xA00 | asset | Icon/Titel-Bereich, Inhalt nicht dekodiert |
| 0x30DA00 | 0x1F5F725 | 0x1C51D25 | asset | 598 benannte Dateien unter `data/` |
| 0x1F5F725 | 0x2000000 | 0xA08DB | unclassified | Hinter dem belegten Bereich, aber **kein** einheitliches Padding |

---

## Key Findings

### Architecture

- Das Spiel lädt Module über zwei Overlay-Slots. Overlay 0 ist nur ein Platzhalter: 0x40 Byte plus 0x1FFC0 BSS, zusammen genau 0x20000. Das entspricht dem Abstand von Slot 1 (0x021638A0) zu Slot 2 (0x021838A0). Dass er so den ersten Slot reserviert, ist eine Vermutung und noch nicht im Code belegt.
- Die Modulparameter liegen an 0x02000B68; der Startcode lädt genau diese Adresse als Literal.
- Der Code ist fast nur ARM: Im direkten Trace stehen 19.740 ARM- gegen 18 Thumb-Befehle.

**Startablauf ab 0x02000800** (aus der Disassemblierung):

1. Interrupts aus (schreibt nach 0x04000208), wartet, bis 0x04000006 null ist.
2. `0x02000A78` setzt CP15: Protection Regions, Caches, ITCM und DTCM.
3. Stacks für IRQ-, SVC- und System-Modus im DTCM.
4. `0x0200093C` löscht DTCM, Paletten-RAM und OAM (Rolle aus den Argumenten abgeleitet, die Routine selbst nicht gelesen).
5. `0x02000950` entpackt das Hauptprogramm. `0x020009FC` kopiert die Autoload-Daten und löscht deren BSS.
6. Löscht das static BSS, ruft `0x020831D8`, `0x02000B60` und `0x020832A0` auf (Rolle offen).
7. `bx` nach **0x02003000** mit `lr` = 0xFFFF0000.

**Hauptfunktion 0x02003000:** ruft einmal `0x02016C20` auf, danach endlos `0x02065310`, `0x01FFC324` und, wenn das nicht 0 liefert, `0x02016D50`. Die Rollen dieser Funktionen sind noch offen. Der Name `main` ist abgeleitet: Es ist das Sprungziel am Ende des Startcodes und enthält eine Endlosschleife.

**Bestätigung der Entpackung:** `0x02000950` ist die Entpackroutine des Spiels. Footer, Bitreihenfolge, Länge (obere 4 Bit + 3) und Distanz (untere 12 Bit + 3) stimmen mit `blz_decompress` in `tools/decompress.py` überein. Das stützt ARM9 und Overlays zusätzlich durch den Code selbst: `start_overlay` (0x02069784) ruft für Overlays dieselbe Routine auf.

**Overlay-Lader** (Kette von unten nach oben):

| Adresse | Funktion | Was sie tut |
|---------|----------|-------------|
| 0x02069564 | `load_overlay_info(info, cpu, id)` | Liest den 32-Byte-Eintrag `id << 5` aus einer vorgeladenen Tabelle oder über die Header-Kopie bei 0x027FFE50 |
| 0x02069660 | `read_overlay_image(info)` | Liest das Overlay in den RAM |
| 0x02069784 | `start_overlay(info)` | Bei `0x027FFC40 == 2` und Flagbit 25: Prüfung gegen 20-Byte-Prüfsummen ab 0x020BD408. Bei Flagbit 24: `blz_decompress`. Danach Cache-Pflege (Vermutung für 0x02064024) und Static-Init-Tabelle |
| 0x02069978 | `load_overlay(cpu, id)` | Ruft die drei obigen auf |
| 0x020699CC | `unload_overlay(cpu, id)` | Führt registrierte Callbacks für den Adressbereich aus |

`load_overlay` und `unload_overlay` werden im gesamten Code nur je einmal direkt aufgerufen, beide aus dem Szenen-Code unten.

**Spielmodi und Szenen:**

- Es gibt 157 Modi (0x00 bis 0x9C). Modi ab 0x2C haben ein Overlay; für alle 113 gilt `Szene = Modus - 0x2C`. Die Zuordnung steht in `modes.csv` und `scenes.csv`.
- Von den Modi unter 0x2C haben nur 11 eine Einstiegsfunktion, alle im Hauptprogramm: 0x00, 0x01, 0x16, 0x17, 0x25 bis 0x2B.
- `scene_manager` (0x0201A448) läuft in einer Schleife über den Zustand bei 0x020C9660: `+0x00` aktuelle Szene, `+0x0C` vorheriger Modus, `+0x0D` nächster Modus, `+0x0F` aktueller Modus.
- **Ablauf je Modus:** Szene laden (`load_scene`), Task mit Phase 1 starten (`scene_setup_task`, 0x800 Byte Stack), danach Task mit der Einstiegsfunktion (Phase 0, 0x1000 Byte Stack), danach Phase 2, zuletzt `unload_scene`. Dass `0x0201E438` Tasks anlegt, ist aus den Argumenten abgeleitet.
- **Zwei Overlay-Slots:** `scene_slot` (0x0201CF44): Szene unter 78 = Slot 0 (0x021638A0), sonst Slot 1 (0x021838A0). Geladene Szene je Slot steht als s16 bei 0x020CEFB4. Slot 1 kann unabhängig geladen werden, das tut auch `0x0202D2EC`, also liegen dort vermutlich Module, die neben einem anderen Modus laufen.
- **`set_next_mode`** (0x0201A3AC) wird 263-mal direkt aufgerufen, viele davon mit konstantem Modus. Daraus lässt sich der Spielablauf als Graph gewinnen.
- **Sonderfälle:** Overlay 0 (0x40 Byte) gehört zu den Szenen 0, 9, 16, 51, 63. Overlay 84 teilen sich die Szenen 88 und 89. Die Funktionen von Szene 93 liegen im Hauptprogramm ab 0x02001F94 und rufen von dort direkt in Overlay 88.

### Kompression

Alles ist mit `tools/decompress.py` nach `unpacked/` entpackt (1521 Dateien, 46,8 MB, Einzelergebnisse in `unpack_report.csv`). Keine Datei schlug fehl.

| Art | Anzahl | Verfahren | Prüfung |
|-----|--------|-----------|---------|
| ARM9 | 1 | BLZ, ohne die ersten 0x4000 Byte | 0x72F88 → 0xC2F58; Ende passt zu `autoload_list_end`, Autoload-Größen ergeben genau 0x52C0 |
| Overlays | 107 | BLZ | Zeiger treffen sich am Ende, Größe = `ram_size` der Overlay-Tabelle |
| Overlay 0 | 1 | nicht komprimiert | Größe = `ram_size` |
| Dateien | 463 | LZ10 | Größe stimmt, 0 Byte Rest |
| Dateien | 50 | Huffman, 8 Bit (Tag 0x28) | Größe stimmt, 0 Byte Rest |
| Container | 50 | siehe unten | Tabelle passt bei allen 50 |
| Container-Einträge | 837 | LZ10 | Größe stimmt |
| Ohne Kompression | 35 | ohne `_LZ` im Namen | unverändert kopiert |

- **Container-Format (bestätigt):** `u32 N`, dann N Paare `(u32 Offset, u32 Größe)`. Die Offsets zählen ab Byte 4, die Einträge folgen lückenlos in Reihenfolge, auf 4 Byte ausgerichtet. Entpackt liegen sie in `<Dateiname>.d/NNN.bin`.
- **Der Dateiname sagt nichts über das Verfahren:** `_LZ` steht auch an Huffman-Dateien und Containern. Entscheidend ist das erste Byte, und selbst das ist bei Containern nur die Eintragszahl.
- **Vorsicht bei `nds_header.py`:** Die Spalte `magic` rät nur anhand des ersten Bytes. `HUFF8?` stimmt, `HUFF4?` nicht (beide Dateien sind Container mit 0x24 Einträgen). `unpack_report.csv` ist die verlässliche Quelle.
- **Secure Area:** ARM9 beginnt mit `E7FFDEFF E7FFDEFF`. Der Bereich liegt also entschlüsselt vor.
- **Vergleich mit dem laufenden Spiel (melonDS, `tools/verify_ram.py`):** Hauptprogramm 777.339 von 777.344 Byte gleich; die 5 abweichenden Bytes liegen bei 0x020AE5FC und 0x020AEBF4, also hinter dem Code. ITCM 21.024 von 21.024 gleich. Das geladene `ov105` 21.504 von 21.504 gleich. DTCM 151 von 160, dort sind Änderungen im Betrieb zu erwarten. Damit ist die BLZ-Entpackung am echten Spiel bestätigt; verglichen wurde bisher nur dieses eine Overlay.
- **Daten beim Laden (`trace_loads.csv`):** Alle 78 Palette- und Tile-Aufrufe aus Dateien im ersten Mitschnitt stimmen per SHA-1 mit einem Blatt in meinem Index überein, dazu 2 Maps. 26 weitere Aufrufe zeigen auf im Hauptprogramm eingebettete Daten. Das bestätigt das Auflösen von Containern und die Kompression für die dabei gesehenen Dateien; welche Verfahren genau darunter waren, ist nicht ausgewertet.
- **Grenze der Prüfung:** Nur für das bisher Gesehene. Die meisten Dateien wurden noch nicht geladen. Beim Overlay-Code liegt der Anteil von ARM-Wörtern mit Bedingung „always“ im Median bei 0,85 (Minimum 0,47), was zu ARM-Code passt, aber kein Beweis ist.

### Entpackte Dateitypen

Die Zählung über alle Ebenen steht unter „2D-Grafik“. `HBDF` ist keine 2D-Grafik, sondern ein Rahmen um `MDLF` und `ANMF` (vermutlich 3D-Modelle und Animationen).

- Static-Init-Tabellen: Alle 108 Overlays haben genau einen Eintrag, und der ist null. Die Tabellen sind also leer.

### Dateisystem

- 706 FAT-Einträge: 108 Overlays und 598 Dateien, alle unter `data/`.
- `data/` direkt: 183 Dateien, darunter 135 mit `mg`-Präfix (vermutlich Minispiele), 15 `bmap` und `MPDS_sound.sdat` (3,1 MiB, Signatur SDAT).
- Sprachordner `FR`, `GE`, `IT`, `SP`, `UK` mit je 82 Dateien; `FR` hat 87. Die 5 zusätzlichen sind `bmap_01` bis `bmap_05`. Die übrigen Unterschiede sind nur Namen wie `bd01_map1_fr` und `_uk`.
- Häufigster Präfix in `UK`: `mpd` (35 Dateien). Inhalt noch offen.

### 2D-Grafik

Viele Dateien sind mehrfach verschachtelt: Kompression, Container, wieder Kompression. `tools/asset_walk.py` löst das auf und ordnet jedes Blatt nach Regeln ein, die die exakte Dateigröße prüfen. Ergebnis über 19.259 Blätter (54,4 MB, `assets.csv`):

| Art | Anzahl | Anteil an den Bytes |
|-----|--------|---------------------|
| Tiles, 4 Bit | 3.724 | 23,5 % |
| Tiles, 8 Bit | 641 | 16,7 % |
| Tilemaps | 1.416 | 3,3 % |
| Paletten | 3.950 | 0,9 % |
| Vollbilder 256×192, 16 Bit | 6 | 1,1 % |
| `offsets` (vermutlich Sprite-Beschreibungen) | 5.237 | 1,9 % |
| `HBDF`/`MDLF` (3D-Modelle, vermutlich) | 1.520 | 25,4 % |
| `HBDF`/`ANMF` (Animationen, vermutlich) | 1.958 | 10,8 % |
| `TEXS` | 165 | 2,0 % |
| Text | 472 | 3,1 % |
| `SDAT` (Sound) | 1 | 6,0 % |
| nicht eingeordnet | 169 | 5,4 % |

**Formate:**

| Format | Aufbau | Beleg |
|--------|--------|-------|
| Palette | `u16 Bytes, u16 Farben`, dann BGR555 | Größe; `0x0200724C` liest das Größenfeld |
| Tiles | `u32`: Bit 0-3 Format (0 = 0x20 Byte je Tile, sonst 0x40), Bit 4-7 Kompression, ab Bit 8 Anzahl | direkt aus `0x020072E4` gelesen |
| Tilemap | `u16 Bytes, u8 Breite, u8 Höhe`, dann u16 je Feld (DS-Format: Bit 0-9 Tile, 10/11 Spiegelung, 12-15 Palettenbank) | Größe; Bilder |
| Hülle | `u32`: Bit 4-7 Kompressionstyp, ab Bit 8 Innengröße | `0x020072E4`, Größe |
| Container | wie unter „Kompression“; `get_member` (0x02009994) liest `Offsets[Index * 2] + 4` | direkt aus dem Code |

`load_bg` (0x02009A38) nimmt außerdem einen Palettenversatz (`arg << 5`, ab 0x100 anders behandelt) und einen Tile-Versatz. Maps können also auf Tile-Sätze zeigen, die hinter einem anderen im Videospeicher liegen. Das ist vermutlich ein Teil der geratenen Fälle unten; noch nicht geprüft.

**Paarung Map, Palette, Tiles (`tools/render_screen.py`):** Die Datei legt das nicht fest, die Reihenfolge wechselt (`Map, Palette, Tiles`, `Tiles, Map, Palette`, parallele Listen). Die Regel: der Tile-Satz mit genau `höchster Index + 1` Tiles, die Palette nach Position. Eine rein positionsbasierte erste Version erzeugte sichtbaren Müll, den die Zahlenprüfung nicht zeigte.

| Messung | Ergebnis |
|---------|----------|
| Gerenderte Maps | 1.412 (4 ohne Partner) |
| Paarung aus Datensätzen, die der Lader in `ov001` liest | 56 (46 in Minispielen) |
| Paarung wahrscheinlich (zweite Stufe) | 162 (56 in Minispielen) |
| Regel, Größe passt | 991, **kein Beleg**, siehe unten |
| Regel, geraten | 203, darunter sichtbar falsche wie `mg_126#049` |
| Werte außerhalb des Bereichs | 15 (6 belegt, 9 nach Regel) |
| Eindeutige Bilder | 685 in `gfx/`, durchsuchbar in `web/catalog.html` |

**Datensätze im Code** (`tools/harvest_bg_tables.py`, `bg_tables.csv`): `ov001`, Lader 0x021697A8, Tabelle ab 0x0216E618, 10 Byte je Datensatz, `s8`: `+0` Palette, `+1` Tiles, `+2` Map, `+3` Engine, `+7` Layer. Die Rollen sind aus dem Code gelesen. Ein Treffer braucht die richtigen Dateiarten, Engine 0/1, Layer 0-3 und einen Code-Bezug: Seine Adresse steht als Wort im selben Bild, oder er liegt in einer lückenlosen Kette daneben. Ohne Code-Bezug bleiben von 196 Kandidaten 46 übrig; das zeigt, wie viel Zufall eine reine Byte-Suche liefert. Dieses Format gibt es nur in 14 Dateien, andere Overlays bauen ihre Tabellen anders.

**Map-Indizes dürfen den Tile-Satz überschreiten:** `mg_101#000` zeigt bis Index 255 bei 254 Tiles, `mg_102#038` bis 368 bei 186. Die Bilder sehen trotzdem richtig aus (Muster, Speedlines).

**Zweite Stufe** (`tools/harvest_bg_generic.py`, `bg_tables_generic.csv`): beliebige Feldreihenfolge und -breite, aber Code-Bezug und exakte Passung Pflicht. Stimmt an 13 von 14 Überschneidungen mit der ersten Stufe überein; das sind wenige Fälle. 17 Paarungen wurden verworfen, weil ihre Farben außerhalb der Palette lagen. Sie heißt deshalb „wahrscheinlich“, nicht „belegt“.

**Die Regel ist schwach:** Gegen die 56 Maps der ersten Stufe stimmen die Tiles in 29 Fällen (52 %), die Palette in 34 (61 %). Die exakte Passung ist also kein Beleg, wie hier zunächst angenommen; bei `mg_101#000` führt sie gezielt in die Irre. Die Stichprobe stammt aus wenigen Dateien, aber für die 1.194 Maps nach Regel sind Fehler zu erwarten, auch wenn viele Stichproben gut aussehen.

**Wie weit das belegt ist:**

- Die Formate sind aus dem Code gelesen oder per exakter Größe geprüft, und Stichproben sehen richtig aus (Logos, Titelbild, Minispielbilder).
- `tools/check_bg_calls.py` verfolgt direkte Ladeaufrufe. Von 214 Map-Ladevorgängen konnte es nur 2 vollständig auflösen (das Logo), beide stimmen. Die übrigen nutzen gespeicherte Zeiger oder berechnete Indizes.
- Korrekte Farben und Überlagerung der Ebenen wurden nicht gegen ein laufendes Spiel geprüft. Der Checkpoint von Phase 4 ist offen.
- Die Bilder zeigen einzelne Ebenen, nicht fertige Bildschirme.

### Sprites

**Frames (`offsets`-Blätter, 2.868):** `u32 N`, `u32 Offsets[N]` ab +4. Frame: `u16 Teile`, je Teil 10 Byte: OAM `attr0`, `attr1`, `attr2`, dann `s16 x`, `s16 y`. Bestätigt durch 69.188 Teile: x stimmt immer mit `attr1` überein, y bis auf 100 mit `attr0`, keine ungültige Form.

**Tile-Index-Einheit** je Satz verschieden (aus der exakten Größe bestimmt): 32 Byte (die meisten), 128 (139 Sätze), 64 (14), 256 (5).

**Animationen (die übrigen `offsets`-Blätter, 2.369):** gleiche Offset-Tabelle, Schritte zu 4 Byte `u8 Dauer, u8 Frame, u16`, Ende bei Dauer 0. Passt bei 2.308. Bei 2.294 von 2.305 direkt hinter einem Frame-Satz liegen alle Frame-Nummern im Bereich. Nicht geklärt: 61 Blätter mit anderem Aufbau (teils Ende mit `FF`), die Bedeutung des u16 und 11 zu hohe Frame-Nummern.

**Deskriptoren im Code:** Das Spiel führt Tabellen mit `s16`-Indizes in einen Container, und zwar in zwei Reihenfolgen: `Tiles, Palette, Frames, Animation` in den Minispielen, `Palette, Tiles, Frames, Animation` in den Menüs. Die Art der Einträge unterscheidet beide eindeutig.

- `ov001` 0x02169B74 liest einen Datensatz zu 12 Byte (`Tiles, Palette, Frames, Animation, -1, 0`) aus der Tabelle ab 0x0216E568. `+0` geht an `0x02020AE0` und `0x02007744`, denselben Tile-Pfad wie in `load_sprite_gfx`. `+4` und `+6` werden geholt und in der Struktur abgelegt; dass sie Frames und Animation sind, ist aus den Dateiarten geschlossen.
- Das erste Argument wählt einen Container aus einer Liste (`[0x0216E8A0 + 0x18]`, 12 Byte je Eintrag). Minispiele nutzen so auch gemeinsame Dateien, die sie selbst nicht nennen, vor allem `mg_000`. Die Tabelle in `ov001` wechselt zwischen `mg_101` und `mg_000`; alle 12 Datensätze sind gültig.
- `0x02009C74(engine, Palette, Tiles, ...)` lädt Sprite-Palette und -Tiles für Stellen, die keine solche Tabelle verwenden.
- `tools/harvest_sprite_tables.py` sucht die Tabellen: 1.818 Treffer (`sprite_tables.csv`). Ein Treffer verlangt die richtigen Dateiarten, exakt passende Frames und eine Animation oder -1 im vierten Feld. Treffer im Hauptprogramm sind mehrdeutig, weil es viele Dateien nennt, und werden nicht verwendet.
- Eine Lockerung ohne Bedingung ans vierte Feld (`tools/check_weak_descriptors.py`) brächte nur 13 weitere Sätze und weicht an belegten Sätzen in 93 von 773 Fällen ab. Sie wird nicht verwendet.

**Ergebnis** (`tools/render_sprites.py`, `gfx_sprites.csv`):

| Messung | Ergebnis |
|---------|----------|
| Gerenderte Frame-Sätze | 2.867, davon 1.522 eindeutige Bilder (bis 64 Frames je Bild) |
| Paarung vom Spiel belegt | 1.836, davon 66 mit mehreren gültigen Paletten (eine wird gezeigt) |
| Paarung nach Regel | 1.031 |
| Minispiele belegt | 447 von 716 |
| Werte außerhalb des Bereichs | 15, alle mit Deskriptor (`party_mode#113`, `puzzle_mode#041`, `title#028` je Sprache) |

Der Vergleich der Regel mit den Belegen steht in der nächsten Tabelle.

| Regel gegen Belege (eindeutige Fälle, Vergleich nach Inhalt) | Fälle | Tiles gleich | Palette gleich |
|------------------------------------------------------------|-------|--------------|----------------|
| Minispiele | 434 | 420 (97 %) | 363 (84 %) |
| Übrige | 1.336 | 1.336 (100 %) | 1.261 (94 %) |

Für die 1.031 Sätze nach Regel sind diese Quoten nur ein Anhaltspunkt: Belegte Sätze können anders aufgebaut sein als unbelegte.

**Grenzen:**

- 269 Minispiel-Sätze bleiben ohne Beleg, am meisten in `mg_118` (31), `mg_503` (24), `mg_501` (12). Sie stehen offenbar nicht in solchen Tabellen, vermutlich laden sie über Konstanten im Code. Noch nicht untersucht.
- Die 15 Sätze außerhalb des Bereichs deuten darauf hin, dass der Palettenplatz zur Laufzeit vergeben wird (`0x02020DBC`, Ergebnis `<< 5`). Nicht geprüft.
- Ein falscher Zwischenstand zeigt die Grenze der Bereichsprüfung: Eine fehlerhafte Regel lag bei 60 % und meldete trotzdem fast nichts außerhalb des Bereichs.

### Minispiele

Die vollständige Liste steht in `MINISPIELE.md` und `minigames.csv` (mit allen fünf Sprachen, Anleitung und Steuerung).

**Stammtabelle bei 0x020A216C:** 78 Einträge zu 24 Byte, Index = Szene. `0x0202A060` liefert den Eintrag zu einem Index.

| Offset | Inhalt |
|--------|--------|
| +0x00 | Flags |
| +0x04 | Titel, Text-ID aus Datei 0x12 |
| +0x08 | Anleitung, Text-ID aus Datei 0x10 |
| +0x0C, +0x10 | Steuerung, Text-IDs aus Datei 0x11 |
| +0x14 | Tipp, Text-ID aus Datei 0x15 |

**Text-IDs:** `(Datei << 16) | Nachricht`, gelesen von `get_message` (0x020187F4). Die Dateinummer ist der Index in die Tabelle bei 0x02098BA0, je Sprache 35 Einträge. `get_mpd_path` (0x02018584) wählt die Sprache über das Feld `+0xCC` von 0x020C941C.

**Format der `mpd_*`-Dateien (bestätigt an allen 175):** `u32 N`, `u32 Offsets[N]`, danach UTF-16LE. Steuercodes im Text: `[cN]...[/c]` (Farbe, vermutlich), `[w]`, `[f]`, `[name1]`, `[e000]`.

**Warum die Zuordnung belegt ist:**

- Die 4 Lücken der Tabelle (Szenen 9, 16, 51, 63) und der als `0xFFFFFFFF` markierte Eintrag von Szene 0 sind genau die 5 Szenen mit Overlay 0.
- Tabellenanfang und -ende stehen als Literale im Code (0x020A216C, 0x020A28BC).
- Die 73 Titel-IDs decken 0 bis 72 je genau einmal ab. **Die Reihenfolge weicht aber ab:** „All Geared Up“ (`mg_134`) hat Titel 58, und die 27 Spiele von `mg_201` bis `mg_402` sind dadurch um eins verschoben. Abzählen wäre also falsch.
- Die `mg`-Nummer stammt unabhängig davon aus den Dateinamen im jeweiligen Overlay.
- Gegenproben: `mpd_096_606inst` (Name enthält 606) handelt von Dreiecken und passt zu `mg_606` „Triangle Twisters“. Die Boss-Texte in `mpd_093_bossmess` laufen in derselben Reihenfolge wie `mg_501` bis `505`.

**Gruppen:** Das untere Flag-Byte gehört fest zur Hunderterstelle: 1xx = 0xE3/0xA3, 2xx = 0xE7/0xA7, 3xx = 0xEB/0xCB/0xAB, 4xx = 0xAF, 5xx = 0x36, 6xx = 0x3A. Belegt sind Bosse (5xx, `bossmess`) und Puzzles (6xx). Die Texte der 2xx sprechen von drei Gegnern und die der 3xx von einem Partner, das passt zu 1 gegen 3 und 2 gegen 2.

**Entfernte Minispiele:** `mg_109`, `mg_116`, `mg_305` und `mg_403` fehlen. Ihre Modi (0x35, 0x3C, 0x5F, 0x6B) zeigen auf das leere Overlay 0, und ihre Einträge sind leer. Dass sie geplant waren, ist nur aus den Nummernlücken geschlossen.

**Die Flags sind nicht vollständig entschlüsselt.** Das oberste Byte hängt mit der Steuerung im Steuerungstext zusammen: 0x08 = Mikrofon (genau die 4 Spiele 119, 212, 312, 404), 0x04 und 0x05 = immer Stylus (22 Spiele), 0x02 und 0x03 = nie Stylus (44 Spiele). 0x06 (2 Spiele, eines mit Stylus) und 0x07 (1 Spiel) sind offen. Die Zugriffsfunktionen lesen außerdem die Bitfelder 19-21 und 22-24 (0x0202A1C0, 0x0202A1E0), deren Bedeutung noch fehlt.

**Unschärfe:** Overlay 61 nennt neben `mg_405` auch `mg_209`. Dort steht in der Liste 405, weil es das letzte Element ist und zur Reihenfolge der Modi passt.

### Disassembler und Trace

- `tools/instruction_set.py` besteht 93 von Hand kodierte Befehle sowie einen Fuzz-Test ohne Absturz (alle 65.536 Thumb-Werte, 500.000 zufällige ARM-Wörter). Die Erwartungswerte stammen von mir, ein Vergleich mit einem unabhängigen Disassembler oder Emulator fehlt.
- **Direkter Trace nur ab dem Einstieg:** 19.758 Befehle, 778 Funktionen, **0 undefinierte Befehle**, 0 Moduskonflikte, Abdeckung 9,9 %.
- **Hauptprogramm mit den 11 Modus-Einstiegen** (`functions.csv`): 53.302 Befehle (71 Thumb), 1.726 Funktionen, **0 undefinierte Befehle**, 0 Konflikte, Abdeckung 26,7 %. 118 indirekte Sprünge bleiben ungelöst; der Rest wird wohl über Funktionszeiger erreicht.
- **Overlay-Trace** (`tools/decode_scenes.py`, `overlay_trace.csv`): von den Szenenzeigern aus 361.069 ARM-Befehle, **0 undefinierte**, 0 Thumb, Abdeckung 36,3 % (Median je Overlay 42,6 %). Am schwächsten: Overlay 88 (0 %, kein eigener Einstieg, siehe Szene 93) und die Overlays 75 bis 79 (1,5 %).
- **Prüfung der Tabellen gegen den Code:** 316 von 319 Szenenzeigern liegen im Overlay ihrer Szene, die übrigen 3 sind Szene 93. Alle 108 Overlays werden verwendet. Die Slot-Regel stimmt bei allen 113 Szenen. Die Einstiegsfunktion aus 0x02099BFC ist bei allen 113 identisch mit Phase 0.
- **Trace mit `--pointers`:** Abdeckung 20,8 %, aber 58 undefinierte Befehle. Das sind Daten, die als Code gelesen wurden (z. B. 0x77656956 = „View“). Diese Stufe rät und ist nur als Kandidatenliste brauchbar; ihre Prozentzahl ist überhöht.
- **ARM7:** Der Trace endet nach 0,4 %, weil der Code nach dem Start in andere Speicherbereiche springt, die `nds_image.py` für ARM7 noch nicht abbildet.
- **`xref.py` liefert nur Kandidaten:** Es scannt alle Positionen in beiden Befehlssätzen. Beispiel: Der `thumb-b`-Treffer bei 0x020034CE auf `main` liegt mitten in ARM-Code und ist falsch.

### Data Structures

| Adresse | Inhalt | Eintrag |
|---------|--------|---------|
| 0x02099B18 | Szene je Modus, ab Modus 0x2C | s16, 113 Einträge plus 1 Füllwert |
| 0x02099BFC | Einstiegsfunktion je Modus | u32, 157 Einträge |
| 0x02099EE8 | Overlay je Szene | u32, 113 Einträge |
| 0x0209A0AC | Funktionen je Szene | 3 × u32 (Phase 0, 1, 2), 113 Einträge; Phase 1 und 2 je 10-mal null |

Overlay-Tabelle in der ROM: 32 Byte je Eintrag. Das Feld bei `+0x1C` enthält in Bit 0-23 die Größe der komprimierten Daten, Bit 24 = komprimiert, Bit 25 = Prüfsumme prüfen. Die Bedeutung von Bit 25 ist aus `start_overlay` abgeleitet.

### State Machine

*(noch offen)*

---

## Intermediate Output Files

| File | Contents |
|------|----------|
| `nds_files.csv` | Alle FAT-Einträge: ID, Pfad, ROM-Offset, Größe, geratene Signatur |
| `nds_overlays.csv` | Overlay-Tabelle: RAM-Adresse, Größen, Datei-ID, Kompression |
| `unpack_report.csv` | Ergebnis je Datei: Größen, Verfahren, Status |
| `functions.csv` | Funktionen des Hauptprogramms aus dem direkten Trace mit allen Modus-Einstiegen: Adresse, Modus, Herkunft, Aufrufer |
| `assets.csv` | Alle Blätter über alle Ebenen: Name, Art, Größe |
| `gfx/screen_*.png` | Gerenderte Tilemaps und Vollbilder |
| `gfx_screens.csv` | Je Map: gewählte Tiles und Palette, Prüfwerte |
| `gfx/sprite_*.png` | Gerenderte Sprite-Sätze, ein Bild je Satz |
| `gfx_sprites.csv` | Je Satz: Tiles, Palette, Einheit, Quelle (Deskriptor oder Regel) |
| `sprite_tables.csv` | Gefundene Deskriptoren im Code |
| `bg_tables.csv`, `bg_tables_generic.csv` | Gefundene Hintergrund-Datensätze, erste und zweite Stufe |
| `web/catalog.html` | Katalog zum Durchsehen (`python -m http.server 8000`, nur lokal) |
| `MINISPIELE.md` | Alle 73 Minispiele: Nummer, Modus, Overlay, englischer und deutscher Name |
| `minigames.csv` | Dasselbe mit allen Sprachen, Flags, Anleitung und Steuerung |
| `overlay_files.csv` | Je Overlay die im Code genannten Dateien und die `mg`-Nummer |
| `file_refs.csv` | Alle Dateinamen im Code mit ihren Verweisen |
| `modes.csv` | Modus, Szene, Overlay, Einstiegsfunktion |
| `scenes.csv` | Szene, Overlay, Funktionen je Phase, Prüfstatus |
| `overlay_trace.csv` | Abdeckung je Overlay aus dem Trace |
| `unpacked/arm7.bin` | ARM7, unverändert aus der ROM, lädt nach 0x02380000 |
| `unpacked/arm9.bin` | ARM9 entpackt, lädt nach 0x02000000 |
| `unpacked/overlay/ovNNN.bin` | Overlays entpackt, Ladeadresse in `nds_overlays.csv` |
| `unpacked/data/` | Datendateien entpackt, Container als `<Name>.d/NNN.bin` |

---

## Verification Checklist

- [ ] Ph3: 3+ functions traced and cross-checked against emulator trace
- [ ] Ph4: 5+ sprites/tiles extracted and visually compared to emulator
- [ ] Ph5: key data struct confirmed in emulator memory dump, all fields match
- [ ] Ph6: full game session played, no major logic gaps found
- [ ] Ph7: web port pixel-compared against emulator screenshots

---

## Reference Resources

- `tools/nds_header.py`: Header, Overlays, Dateisystem
- `tools/check_layout.py`: ROM-Bereiche, Kompressions-Footer, `_LZ`-Dateien
- `tools/decompress.py`: BLZ, LZ10, Huffman, Container; schreibt `unpacked/`
- `tools/check_unpacked.py`: Signaturen und Plausibilität der entpackten Dateien
- `tools/instruction_set.py` und `tools/test_instruction_set.py`: Decoder und Selbsttest
- `tools/nds_image.py`: Adressraum, Overlays, Labels
- `tools/disasm.py`: Disassembler (heißt nicht `dis.py`, siehe `CLAUDE.md`)
- `tools/search_bytes.py`, `tools/xref.py`: Bytesuche, Querverweise
- `tools/trace_code.py`: Code-Trace ab dem Einstieg
- `tools/asset_walk.py`, `tools/dump_asset.py`, `tools/survey_assets.py`, `tools/survey_other.py`: Dateien auflösen und untersuchen
- `tools/gfx.py`, `tools/render_screen.py`, `tools/contact_sheet.py`, `tools/gen_catalog.py`: Grafik ausgeben
- `tools/check_bg_calls.py`, `tools/debug_bg_sim.py`: Paarung gegen den Spielcode prüfen
- `tools/render_sprites.py`, `tools/survey_sprites.py`, `tools/check_anims.py`: Sprites und Animationen
- `tools/harvest_sprite_tables.py`, `tools/compare_sprite_rule.py`, `tools/check_weak_descriptors.py`: Deskriptoren sammeln, Regel daran messen
- `tools/dump_words.py`: Speicher als s16-Werte ansehen; `tools/find_strings.py`: Texte in einer Datei suchen
- `tools/gdb_server.py`, `tools/gdb_client.py`, `tools/verify_ram.py`, `tools/check_alive.py`: Verbindung zu melonDS und Prüfungen am laufenden Spiel
- `tools/trace_loads.py`, `tools/score_trace.py`: Ladevorgänge mitschreiben und auswerten. Die Option `--modes` löscht das Task-Flag und ließ das Spiel hängen; nicht verwenden, stattdessen Tasten über `KEYINPUT` (`0x01FFC754`) nachbilden
- `tools/survey_port.py`, `tools/survey_wireless.py`: Messwerte für `MULTIPLAYER_ROADMAP.md`
- **Neues Ziel:** native PC-Version mit Online-Multiplayer, siehe `MULTIPLAYER_ROADMAP.md`
- `tools/harvest_bg_tables.py`, `tools/harvest_bg_generic.py`, `tools/discover_bg_layouts.py`: Hintergrund-Datensätze suchen und die Regel daran messen
- `tools/decode_mpd.py`: Textdateien lesen; `tools/decode_minigames.py`: Minispieltabelle
- `tools/map_file_refs.py`, `tools/map_overlay_files.py`: Dateinamen im Code und je Overlay
- `tools/hexdump.py`, `tools/dump_mg_table.py`: Hilfen zum Ansehen von Rohdaten
- `tools/find_refs_range.py`: Wörter mit Werten in einem Adressbereich
- `tools/find_arg_calls.py`: Aufrufe einer Funktion samt konstantem Argument
- `tools/decode_scenes.py`, `decode_modes.py`, `check_scene_slots.py`, `check_mode_phase.py`: Tabellen dekodieren und prüfen
- Emulator für die Validierung: **melonDS 1.1** über seinen GDB-Stub (Entscheidung 2026-11-05). Liegt in `F:\Tools\melonDS\melonDS.exe`, x86_64, SHA-256 des ZIP `9f3f8a24...d4f8c8` wie bei GitHub angegeben, nicht signiert. Die Programmtexte enthalten „Enable GDB stub“ und die Anzeige, dass der Stub nicht zusammen mit dem JIT läuft; der Stub ist also in diesem Build. Getestet: Das Spiel läuft, Register und Speicher lassen sich lesen, das Spiel läuft nach dem Anhalten weiter (`tools/check_alive.py`). Code-Breakpoints funktionieren (`tools/trace_loads.py`, Schritt vom Breakpoint: löschen, Einzelschritt, neu setzen). Ein erster Mitschnitt (107 Aufrufe, 143 s) endete, als melonDS neu gestartet wurde; ob das Spiel vorher hing, ist offen.
- **Eigenheiten des Stubs, an melonDS 1.1 unter Windows beobachtet:** (1) Das erste Byte muss `+` sein; erst nach dem `+` des Stubs dürfen Pakete kommen. (2) Falsche Bytes führen zur Trennung und blockieren danach jede neue Verbindung bis zum Neustart; aus dem Quelltext geschlossen, passt zu den Messungen. (3) Nur eine Verbindung je Start, weil der Stub das Trennen nicht bemerkt; deshalb `tools/gdb_server.py`. (4) Nach dem Anhalten kommen zwei Stopp-Meldungen (`S05`, `S02`), die zweite muss entfernt werden, sonst sind alle Antworten versetzt. (5) Antworten sind auf `PacketSize=47F` begrenzt, also höchstens 0x23F Byte je Lesezugriff. (6) Im ruhenden Spiel hält es oft an derselben Stelle (0x02063B68, Ende der Interrupt-Rückkehr), im laufenden Spiel an wechselnden. Die Stelle ist also nicht vom Stub festgelegt. Stub laut PR #1583: ARM9 auf Port 3333, ARM7 auf 3334 (nur aus einem frühen Kommentar), Register und Speicher lesen und schreiben, Einzelschritt, Code-Breakpoints, keine Daten-Watchpoints, JIT muss aus sein. Reserve: DeSmuME (Lua mit `memory.registerexec`/`registerwrite`), no$gba v3.06 (Schreib-Breakpoints von Hand, keine Automatisierung)
- Warum Code-Breakpoints reichen: `load_palette`, `load_tiles`, `load_map`, `load_bg`, `load_overlay` sind bekannt. Ein Breakpoint dort zeigt die tatsächlich übergebenen Daten, und über deren Inhalt lässt sich das Blatt bestimmen

---

## Next Tasks

### RE Investigation

- [x] Ph1: identify binary header and platform
- [x] Ph2: `tools/decompress.py` mit BLZ schreiben, ARM9 entpacken und Größe 0xC2F58 bestätigen
- [x] Ph2: alle 107 komprimierten Overlays mit BLZ entpacken und gegen `ram_size` prüfen
- [x] Ph2: LZ10 ergänzen und die 463 Dateien mit Tag 0x10 gegen ihre Header-Größe prüfen
- [x] Ph2: Container-Hypothese an den 100 übrigen `_LZ`-Dateien prüfen (50 Container, 50 Huffman)
- [x] Ph4: `HBDF` einordnen (Rahmen um `MDLF`/`ANMF`, keine 2D-Grafik)
- [x] Ph4: Paletten-Hypothese prüfen (bestätigt, Paletten gibt es auch mit anderen Größen)
- [ ] Ph1: Bereich 0x1F5F725-0x2000000 einordnen (Inhalt und ob das Spiel darauf zugreift)
- [x] Ph3: `tools/instruction_set.py` für ARMv5TE und Thumb schreiben
- [x] Ph3: `tools/disasm.py`, `search_bytes.py` und `xref.py` mit Overlay-Unterstützung bauen
- [x] Ph3: Einstieg ab 0x02000800 bis zur Hauptfunktion verfolgen, Autoload-Tabelle auswerten
- [x] Ph3: Emulator festlegen (melonDS 1.1, siehe Reference Resources)
- [x] Ph3: melonDS laden und prüfen, ob der Windows-Build den Stub enthält
- [x] Ph3: JIT aus, Stub aktivieren; prüfen, ob das Spiel läuft
- [x] Ph3: `tools/gdb_client.py` und `tools/gdb_server.py` schreiben (nur Standardbibliothek)
- [x] Ph3: RAM mit `unpacked/arm9.bin` und dem geladenen Overlay vergleichen (`tools/verify_ram.py`)
- [ ] Ph3: Code-Breakpoints testen, dann 3+ Funktionen gegen einen Trace prüfen (Checkpoint von Phase 3)
- [ ] Ph3: Weitere Overlays beim Laden vergleichen, Breakpoint auf `start_overlay`
- [ ] Ph4: Aufrufe von `load_palette`, `load_tiles`, `load_map` mitschreiben und damit die 1.194 Tilemaps nach Regel prüfen; testen, ob Schreiben von `mode_state+0xD` Modi direkt anspringen lässt
- [x] Ph3: Overlay-Lader finden und klären, welches Overlay wann in welchen Slot kommt
- [ ] Ph3: Modusgraph aus den 263 Aufrufen von `set_next_mode` erzeugen, dynamische Fälle auflösen
- [x] Ph5: Minispiele den Modi und Overlays zuordnen
- [ ] Ph5: Übrige Modi benennen (0x7A bis 0x9C anhand von `overlay_files.csv`, darunter Menüs und Spielbretter)
- [ ] Ph5: Flags der Minispieltabelle entschlüsseln
- [ ] Ph5: Tabellen ab 0x020A1FA0, 0x020A207C und 0x020A28BC einordnen, sie liegen direkt neben der Minispieltabelle
- [ ] Ph3: Overlays 75 bis 79 untersuchen (nur 1,5 % Abdeckung bei 22 Funktionen)
- [ ] Ph3: Hauptschleife klären: 0x02016C20, 0x02065310, 0x01FFC324, 0x02016D50
- [ ] Ph3: Weitere häufig gerufene Funktionen benennen: 0x02027110, 0x0205E67C, 0x02060C28, 0x02065C8C
- [ ] Ph3: Abdeckung erhöhen: Funktionszeiger-Tabellen finden, die 40 ungelösten indirekten Sprünge auswerten
- [ ] Ph3: `--pointers` verbessern, sodass Daten nicht mehr als Code durchgehen
- [ ] Ph3: ARM7-Relokation in `nds_image.py` abbilden
- [x] Ph4: Grafikformat der `mg_*`-Daten bestimmen
- [x] Ph4: Sprites darstellen: `offsets`-Blätter entschlüsseln
- [x] Ph4: Minispiel-Deskriptoren auswerten (447 von 716 Sätzen belegt)
- [ ] Ph4: Die übrigen 269 Minispiel-Sätze belegen, Einstieg über `mg_118` und `mg_503`
- [x] Ph4: `s8`-Datensätze für Hintergründe auswerten (56 belegt, 162 wahrscheinlich)
- [ ] Ph4: Weitere Tabellenformate für Hintergründe finden: Die 1.194 Maps nach Regel sind der größte offene Punkt. Einstieg: Lader je Overlay über Aufrufe von `load_tiles` und `load_map` suchen, Beispiel `mg_126`
- [ ] Ph4: 61 abweichende Animationsblätter und das u16-Feld klären, Animationen im Katalog abspielen
- [ ] Ph4: Frame-Leser im Spielcode finden, der Teile zu 10 Byte in OAM schreibt
- [ ] Ph4: Tile- und Palettenversatz aus `load_bg` berücksichtigen, `check_bg_calls.py` auf gespeicherte Zeiger erweitern
- [ ] Ph4: Vollständige Bildschirme aus mehreren Ebenen zusammensetzen
- [ ] Ph4: `TEXS` und die 169 nicht eingeordneten Blätter untersuchen
- [ ] Ph5: `MDLF` und `ANMF` entschlüsseln (Spielfiguren und Modelle)
- [ ] Ph5: Sound: `MPDS_sound.sdat` auslesen
- [ ] Ph4: Checkpoint: 5+ Assets gegen einen Emulator vergleichen
- [ ] Ph5: Zuordnung Minispiel zu Overlay und `mg_*`-Dateien aufstellen
- [x] Ph5: Format der `mpd_*`-Dateien in den Sprachordnern bestimmen
- [x] Ph5: Textformat und Zeichenkodierung finden
- [ ] Ph5: Steuercodes im Text klären (`[cN]`, `[w]`, `[f]`, `[eNNN]`)

### Web Port Fixes

### Documentation
