# Mario Party DS: native PC-Version mit Online-Multiplayer

Stand: 2026-11-05. Status: **Analyse und Architekturvorschlag. Noch nichts davon ist gebaut.**

Kennzeichnung: **[gemessen]** stammt aus Werkzeugen dieses Projekts, **[Hypothese]** ist ungeprüft, **[Schätzung]** ist eine grobe Einschätzung.

## 1. Kurzfassung

- **Es gibt noch keine Engine und keine Decompilation.** Das Projekt besteht aus Python-Analysewerkzeugen, Notizen und extrahierten Daten. Es gibt 0 native Quelldateien. Vom Spiel existiert nur der ARM-Maschinencode aus der ROM.
- **Das Spiel hat bereits einen Vier-Spieler-Funkmodus.** Das könnte den Netzwerkteil stark vereinfachen, denn Spiellogik und Abgleich wären schon vorhanden. Wie viel vom Spiel darüber läuft, ist die wichtigste offene Frage.
- **Empfehlung:** ARM-Code automatisch nach C++ übersetzen (statische Rekompilierung), die DS-Hardware durch eine eigene Schicht ersetzen und die Funkschicht des Spiels durch eine Internet-Verbindung mit autoritativem Server. Das erhält die Originallogik und alle Minispiele weitgehend ohne Neuschreiben.
- **Aufwand [Schätzung]:** ein Vorhaben über viele Monate, nicht über Tage. Ein spielbarer Stand ist möglich, aber nicht garantiert. Besonders 3D-Grafik und die Abdeckung des gesamten Codes können Überraschungen bringen.

## 2. Ist-Stand [gemessen]

| Bereich | Stand |
|---|---|
| Sprache des Projekts | Python, 51 Werkzeuge mit etwa 5.800 Zeilen; kein Spielcode |
| Hauptprogramm | 798.528 Byte; 53.302 Befehle in 1.726 Funktionen erreicht |
| Overlays | 108 Module, 3.981.568 Byte; 361.069 Befehle erreicht |
| Nicht erreichter Code | Hauptprogramm 26,7 % abgedeckt, Overlays 36,3 %; 428 ungelöste indirekte Sprünge. Ein Teil des Rests sind Daten, wie viel ist unbekannt |
| Befehlssatz | Fast nur ARM (71 Thumb-Befehle im erreichten Hauptprogramm) |
| Entpacken | BLZ am laufenden Spiel bestätigt (Hauptprogramm bis auf 5 Datenbytes, ITCM, ein Overlay); LZ10 und Container per SHA-1 an bisher gesehenen Ladevorgängen |
| Spielstruktur | 157 Modi, 113 Szenen, Lade- und Szenenablauf bekannt |
| Minispiele | 73 identifiziert: 32 Nummer 1xx, 12 2xx, 13 3xx, 5 4xx, 5 Bosse, 6 Puzzles |
| 2D-Grafik | Formate gelesen, Bilder und Sprites als PNG; Paarung nur teilweise belegt (Sprites 1.836 von 2.867, Tilemaps 56 belegt plus 162 wahrscheinlich von 1.412) |
| 3D | `MDLF` 1.520 und `ANMF` 1.958 Blätter, `TEXS` 165; Format unbekannt |
| Sound | `SDAT` mit 141 Sequenzen, 124 Sequenz-Archiven, 188 Bänken, 3 Wave-Archiven; nicht ausgelesen |
| Text | Alle 175 Textdateien lesbar, 5 Sprachen |
| Emulator zum Vergleich | melonDS 1.1 mit eigenem Client und Code-Breakpoints |
| Spiellogik verstanden | Nur Rahmen: Laden, Szenen, Texte, Tabellen. Würfel, Münzen, Sterne, Züge, Items und Regeln sind **nicht** untersucht |

**Funktionierende Spielmechaniken in nativer Form: keine.** Sie laufen nur im Originalcode.

Fehlgeschlagen: Das Erzwingen eines Szenenwechsels durch Löschen des Task-Flags ließ das Spiel hängen. Tasten-Eingabe über den Emulator: `tools/press_keys.py` setzt bei `0x01FFC3B8` die Tastenmaske in `r9`. Die Tasten sind getestet (vom Titel bis ins Multiplayer-Menü), Touch noch nicht. `tools/grab_window.ps1` nimmt nur das melonDS-Fenster auf.

Phase 0, Stand: Git-Repository angelegt (nur lokal, ROM und Spielinhalte ausgeschlossen). `native/` baut mit MSVC 19.50, CMake und Ninja aus den Build Tools eine `MarioPartyDS.exe`, die ein leeres Fenster zeigt und den automatischen Start-Test besteht. Sie enthält **keinen Spielcode**. SDL3 ist noch nicht eingebunden, weil es dafür einen Download braucht.

## 3. Abhängigkeiten von DS-Hardware [gemessen]

Gezählt sind Datenwörter mit Hardware-Adressen im Code. Das ist eine Untergrenze; einzelne Treffer können Zufall sein.

| Hardware | Wörter | Module | Wird ersetzt durch |
|---|---:|---:|---|
| 2D-Engines A und B | 61 | 13 / 7 | eigene 2D-Darstellung |
| BG- und OBJ-VRAM, Paletten, OAM | 363 | bis 39 | eigener Speicher plus 2D-Darstellung |
| VRAM-Bänke | 56 | 1 | Bank-Abbildung |
| 3D-Register | 121 | 15 | eigene 3D-Darstellung |
| Strom- und 3D-Steuerung | 103 | 54 | Stubs und 3D-Steuerung |
| Rechenbeschleuniger | 59 | 5 | Software-Division und -Wurzel |
| Timer, DMA, Interrupts | 19 | je 2 | Ablaufsteuerung |
| IPC zum ARM7 | 13 | 2 | direkte Ersatzaufrufe, kein ARM7-Code |
| Tasten, Touch, gemeinsamer Bereich | 101 | bis 10 | Tastatur, Controller, Maus |
| Karte | 7 | 2 | Datei-Zugriff, Spielstand |

Der ARM7 (160 KB) enthält nach SDK-Aufbau Sound, Touch, Mikrofon und Funk. **[Hypothese]** Er wird nicht übersetzt, sondern durch PC-Code ersetzt. Direkte Spiel-Registerzugriffe liegen auch in einzelnen Minispiel-Overlays, also kann man sich nicht allein auf SDK-Funktionen stützen.

Eingabe-Bedarf der Minispiele nach Flag und Steuerungstext: mindestens 22 mit Stylus, 4 mit Mikrofon, 44 ohne Stylus; 3 sind ungeklärt.

## 4. Was das Spiel an Mehrspieler schon hat

**[gemessen]**
- Menütext: Party mit bis zu vier Spielern per drahtloser Datenübertragung und einer einzigen DS-Karte.
- Im Hauptprogramm 197 Texte der Nintendo-Funkbibliothek: `WM_` (95), `WH_` (31), `MB_` (24).
- `WH_StateInStartParentKeyShare`, `WH_StateInStartChildKeyShare`, `WM_StartDataSharing`: gemeinsame Daten je Bild über alle Geräte.
- `MB_`: DS-Download-Spiel. Eigene Daten `mbgui_dep`, `mbturn`, `mbpause`, Menü `ov088` (Modus `0x89`).
- Ein Minispiel-Overlay (`ov046`, `mg_302`) enthält `PARENT` und `CHILD`.

### Ergebnis aus Phase 1, aus dem Code gelesen [gemessen]

Das Spiel enthält ein Eingabe-Lockstep:

1. `0x01FFC324` liest Tasten und Touch je Bild und legt sie im eigenen Datensatz ab (`0x020D4700 + 0x98`).
2. `0x02024D44` ist `WH_StepDataSharing`; der Name steht in seiner Fehlermeldung. Es liefert die geteilten Daten nach `0x020D4798`.
3. Für vier Spieler (je 0x26 Byte) werden Tasten und Touch in eigene Strukturen ab `0x020CFCD8` (je 0x38 Byte) übertragen.
4. Liegen keine geteilten Daten vor, gibt die Funktion 0 zurück, und `main` (`0x02003000`) ruft den Spieltakt `0x02016D50` nicht auf.

Die Tabelle für den Download-Start hat Prüfsummen für genau 108 Overlays (`0x020BD408`), und der Code unterscheidet den Start per Download nur an 27 Stellen (`0x027FFC40 == 2`). Es gibt keine eigene Empfänger-Datei in der ROM.

Den Funkzustand (`0x02027110`) fragen 82 Overlays ab: alle 6 Bretter, Party-, Minispiel- und Puzzle-Menüs, 58 von 73 Minispielen. Nicht darunter: 7 der 1xx, 1 der 2xx, 3 der 3xx, 4 von 5 Bossen.

**Folgerung:** Der Austauschpunkt ist klein und klar. Auf dem PC ersetzt eine Funktion `WH_StepDataSharing`, die die Eingaben vom Server holt. Für jede Instanz liegen alle Spieleingaben dann so vor, wie das Spiel sie erwartet. Weil auf dem PC jeder das volle Spiel hat, entfallen außerdem die Speichergrenzen eines Empfänger-DS.

### Ergebnis aus Phase 1, im laufenden Spiel gesehen [gemessen, melonDS]

- Das Menü „Multiplayer“ (Modus `0x89`) setzt das Netz-Flag (`0x020D45A0 + 0x298`) und sucht Spieler. Beim Beenden geht das Flag zurück auf 0.
- Der Puffer ist aktiv: `shared[0]` entspricht dem eigenen Datensatz und ändert sich mit einem Tastendruck. Zustand 1 ist die Suche; der Code behandelt den Austausch ab Zustand 5.
- Spieltext: Party für bis zu vier Spieler, oder Spezialspiele für zwei.
- Ohne Gäste startet der Host keine Partie. Der eigentliche Austausch zwischen Geräten wurde deshalb **nicht** beobachtet.
- Aus dem Code: Der Weg führt nach `0x88`, zur normalen Modusauswahl. `ov089` (Party-Modus) fragt den Funkzustand an 20 Stellen ab.

### Entscheidung

**N1 (Funkschicht ersetzen) ist die Arbeitsgrundlage.** Begründung: Das Eingabe-Lockstep ist im Code belegt, sein Puffer im Betrieb, und der Party-Modus ist offensichtlich darauf ausgelegt. N2 bleibt Rückfall.

**Noch nicht belegt, also Risiken für N1:**
- Dass die Instanzen wirklich dieselbe Logik rechnen. Zufallswerte müssen dabei gleich starten; das ist nicht untersucht.
- Der Umfang. „Fragt den Zustand ab“ bedeutet nicht „läuft im Funkmodus“; auch Story-Menü und Galerie fragen ihn ab. Welche Bretter und Minispiele im Netz angeboten werden, ist nicht gesehen. Falls es nur eine Auswahl ist, fehlen die übrigen Spiele online oder brauchen eigene Arbeit.
- Echter Austausch. Der Empfängerstart braucht die DS-Firmware, die hier nicht vorliegt; eine zweite melonDS-Instanz wurde nicht versucht. Der erste echte Test kommt daher vermutlich erst in Phase 8 oder durch eine eigene Test-Instanz, die den Austausch nachbildet.
- Der Modus für zwei Spieler und die Bosse. Bosse fragen den Zustand größtenteils nicht ab und sind vermutlich Einzelspieler-Inhalte.

## 5. Punkte in der Anforderung, die ich klären muss

| Anforderung | Problem | Vorschlag |
|---|---|---|
| „Bestehende Engine verwenden“, „Stand der Decompilation“ | Beides existiert nicht | Rekompilierung neu aufbauen; die Python-Werkzeuge bleiben als Grundlage |
| „Kein DS-Emulator“ | Die Rekompilierung enthält keinen CPU-Emulator, die Hardwareschicht bildet aber dennoch Grafik und Ablauf des DS nach. Das ist unvermeidbar, wenn die Originallogik bleiben soll | Keine Interpretation von ARM-Code zur Laufzeit; unbekannter Code bricht mit Meldung ab |
| Autoritativer Server und originale Logik | Die Originallogik läuft nur als Ganzes | Der Server rechnet selbst die Partie, ohne Darstellung (Abschnitt 6) |
| TLS mit ENet | ENet nutzt UDP und bietet kein TLS | TLS für die Lobby, dort Sitzungsschlüssel aushandeln; ENet-Pakete damit verschlüsseln |
| Internet für vier PCs | Dafür braucht es einen dauerhaft erreichbaren Server, der laufend Kosten verursacht | Entscheidung nötig: eigener Server, Miet-Server oder Host-PC mit Portfreigabe |
| Alle 73 Minispiele für vier | Einige sind von Natur aus für 1 Spieler (Puzzles, Bosse) oder hängen vom Mikrofon ab | Matrix je Minispiel (Phase 9); Mikrofon über Taste oder echtes Mikrofon |
| Zwei Bildschirme, Touch | PC hat einen Bildschirm | Beide nebeneinander oder übereinander, Maus als Stylus, einstellbar |

### Rechtliches (keine Rechtsberatung)

Das Spiel und seine Inhalte sind urheberrechtlich geschützt. Eine `.exe`, die aus Nintendos Code übersetzt wurde oder Grafiken enthält, darf man nicht einfach weitergeben. Für vier verschiedene PCs ist das direkt relevant. Plan: keine Spielinhalte ins Repository oder in Veröffentlichungen. Ob die Exe den übersetzten Code enthält oder jeder Spieler sie mit dem Installationsprogramm und seiner eigenen, rechtmäßig erworbenen ROM erzeugt, ist eine von dir zu treffende Entscheidung (Abschnitt 10). Der zweite Weg erfordert einen Compiler beim Spieler oder eine aufwendigere Lösung.

## 6. Architektur

### 6.1 Wie der Spielcode nativ wird

| Option | Vorteil | Nachteil | Urteil |
|---|---|---|---|
| A: Vollständige Decompilation nach C | Lesbar, frei änderbar | Rund eine Million Maschinenbefehle von Hand; praktisch Jahre [Schätzung] | Nur für einzelne Funktionen später |
| **B: Statische Rekompilierung nach C++** | Automatisch, erhält die Logik exakt, deterministisch, Funktionen können einzeln durch eigenen Code ersetzt werden | Unlesbarer Ausgangscode; jeder nicht entdeckte Code fehlt; Hardwareschicht ist groß | **Empfehlung** |
| C: melonDS einbetten | Am schnellsten | Widerspricht „kein Emulator“; GPL-Lizenz | Nur als Vergleichswerkzeug |

Vorteilhaft für B: Der Code ist fast nur ARM und aus einem SDK gebaut, mit festen Overlays. Decoder, Overlay-Kenntnis und Tabellen sind vorhanden, melonDS erlaubt den Vergleich Bild für Bild. Probleme: Sprünge über Zeiger und überlappende Overlays (108 Module in zwei Bereichen).

```
ROM des Spielers ─► Extraktor (aus tools/)
ARM-Code ─► Rekompilierer ─► generierter C++-Code je Modul
                                     │
             ┌───────────────────────┴───────────────┐
             │ Sim-Kern: deterministisch, ohne Darstellung │
             │  Speicher, Overlays, Interrupts, Timer,      │
             │  Rechenbeschleuniger, SDK-Ersatz, Funk-Ersatz│
             └──────┬───────────────────────┬───────────┘
              Client│                 Server│ (ohne Fenster)
   2D, 3D, Sound, Eingabe (SDL3)       Partie, Lobby, Prüfung
```

### 6.2 Netzwerk

**Grundidee:** eine Partie ist deterministisch. Gleicher Startwert plus gleiche Eingaben ergeben denselben Zustand. Übertragen werden Eingaben je Bild, kein Spielzustand.

| Variante | Beschreibung | Voraussetzung |
|---|---|---|
| **N1: Funkschicht ersetzen** (bevorzugt) | `WM`/`WH`/`MB` des Spiels werden durch das Netz ersetzt. Der Server läuft als Host-Instanz, die Clients als Empfänger | Funkmodus umfasst genug vom Spiel |
| **N2: Eine gemeinsame Partie** | Alle rechnen eine Einzelkonsole; Eingaben ersetzen die COM-Spieler | Eingriffe je Brett und Minispiel (73), Bildschirm-Ansicht je Spieler |

Für beide:
- **Autorität:** Der Server rechnet die Partie selbst, legt Eingabereihenfolge und Zufallsstartwert fest. Clients senden nur Eingaben. Münzen, Sterne und Würfel entstehen ausschließlich in der Sim, ein Client kann sie nicht festlegen.
- **Abgleich:** Clients senden eine Prüfsumme des Zustands. Bei Abweichung folgt eine Neuübertragung vom Server.
- **Wiedereintritt:** Der Server sendet einen komprimierten Zustand plus die Eingaben seitdem.
- **Verzögerung:** Lockstep wartet auf den Langsamsten. Eine kleine feste Eingabeverzögerung mildert das; Echtzeit-Minispiele sind das Risiko. Rollback ist möglich, weil der Zustand kopierbar ist, aber erst bei Bedarf.
- **Serverausfall:** Der Server speichert regelmäßig Zustand und Eingaben und kann die Partie daraus fortsetzen.

| Baustein | Wahl |
|---|---|
| Echtzeit | ENet über UDP, mit Sitzungsschlüssel verschlüsselt |
| Lobby | WebSocket über TLS |
| Nachrichten | MessagePack für Lobby und Steuerung, kompaktes Binärformat für Eingaben |
| Einladungscode | 6 Zeichen ohne verwechselbare Zeichen, nur auf dem Server vergeben |

### 6.3 Client

C++20, CMake, SDL3 (Fenster, Vollbild, Tastatur, Controller, Maus, Sound). Visual Studio Build Tools 2026 mit C++-Werkzeugen sind installiert [gemessen]; `cmake` steht nicht im Pfad. Online-Menü zunächst als eigene Oberfläche über dem Spiel im Stil des Originals, damit die Original-Menüs unangetastet bleiben.

## 7. Fehlende Funktionen

| Bereich | Fehlt |
|---|---|
| Code | Rekompilierer, fehlende Code-Abdeckung, Sprungtabellen, Overlay-Umschaltung |
| Ablauf | Interrupts, Bildtakt, Timer, DMA, Task-System des Spiels, Rechenbeschleuniger, BIOS-Aufrufe (`swi`) |
| Grafik | 2D-Darstellung komplett (Laufzeit, nicht nur PNG), Effekte, 3D-Geometrie und -Rasterung, Texturen; Formate `MDLF`, `ANMF`, `TEXS` |
| Sound | SDAT-Abspieler: Sequenzen, Bänke, Samples, Mischung |
| Eingabe | Tastatur, Controller, Maus als Stylus, Mikrofon-Ersatz, Einstellungen |
| System | Dateizugriff, Spielstand (`.sav` 8 KB), Uhrzeit, Fenster, Auflösung, Vollbild |
| Netz | Server, Lobby, Codes, ENet, Lockstep, Prüfsummen, Wiedereintritt, Ping-Anzeige, Verschlüsselung, Betrieb |
| Oberfläche | Online-Menü, Raum erstellen und beitreten, Charakterwahl im Netz, Verbindungsanzeige |
| Spielwissen | Funkmodus, Zustand von Brett, Würfel, Münzen, Sternen und Items für Anzeige und Prüfung |
| Tests | Alles aus Abschnitt 9 |

## 8. Phasen

Jede Phase hat ein prüfbares Ziel. Eine Phase gilt erst als fertig, wenn es am echten Lauf bestanden ist. **Phase 1 entscheidet über die Netzarchitektur.**

| Phase | Inhalt | Prüfbares Ziel |
|---|---|---|
| 0 | Git-Repository (ROM und Spielinhalte ausgeschlossen), CMake, Tasten-Eingabe im Emulator. **Erreicht** bis auf SDL3, das in Phase 4 mit Download-Rückfrage folgt | Leeres Fenster als Exe; Tasten wirken im Spiel |
| 1 | **Funkmodus untersuchen:** Datenfluss und Einstieg gelesen, Puffer im Betrieb gesehen, Entscheidung N1 getroffen. **Offen:** Liste der Bretter und Minispiele im Netz, echter Austausch | Teilweise erreicht, siehe Abschnitt 4 |
| 2 | Rekompilierer und minimale Laufzeit, Vergleich gegen melonDS. **Erreicht** (siehe unten) | Start bis `main`: Register und RAM gleich |
| 3 | Code-Abdeckung, Interrupts, Task-System, Overlays. **Erreicht mit Einschränkungen** (siehe unten) | Headless bis ins Hauptmenü, RAM-Abgleich über Hunderte Bilder |
| 4 | 2D-Darstellung, Eingabe, Dateien, Spielstand. **Erreicht mit Einschränkungen** (siehe unten) | Titel und Menüs bedienbar; pixelweiser Vergleich |
| 5 | 3D. **Erreicht mit Einschränkungen** (siehe unten) | Brett und Figuren wie im Emulator |
| 6 | Sound. **Erreicht mit Einschränkungen** (siehe unten) | Musik und Effekte im Vergleich hörbar |
| 7 | Lokale Vollständigkeit. **Erreicht mit Einschränkungen** (siehe unten) | Eine Party lokal von Anfang bis Ende; Minispiel-Matrix lokal |
| 8 | Netz im eigenen Rechner: Lockstep, Server headless | 4 Prozesse lokal, Prüfsummen gleich über eine ganze Partie |
| 9 | Minispiele einzeln online, Matrix mit 73 Zeilen. **Erreicht mit Einschränkungen** (siehe unten) | Jedes Spiel gestartet, gespielt, Ergebnis gleich bei allen; Grenzen dokumentiert |
| 10 | Lobby, Codes, Charakterwahl, Oberfläche. **Erreicht mit Einschränkungen** (siehe unten) | Ablauf von Raum bis Start in 4 Prozessen |
| 11 | Abbrüche, Wiedereintritt, Serverneustart, Verzögerung, Ping. **Erreicht mit Einschränkungen** (siehe unten) | Netzsimulator-Tests bestanden |
| 12 | Internet: Server betreiben, Verschlüsselung, Einstellungen, Vollbild | 4 echte PCs an verschiedenen Standorten spielen eine Party |

### Ergebnis Phase 2 [gemessen, gegen melonDS an `main`]

`tools/recomp.py` übersetzt 53.302 Befehle in 13.120 C++-Blöcke; `MarioPartyDS-headless` startet aus dem komprimierten ROM-Abbild und erreicht `main` in 0,6 s.

| Vergleich | Ergebnis |
|---|---|
| r0 bis r15, CPSR | gleich |
| Hauptspeicher unter `0x02380000` (rund 3,6 MB) | gleich |
| ITCM, DTCM, Palette, OAM | gleich |
| Hauptspeicher ab `0x02380000` | abweichend wie erwartet: ARM7-Code (`0x02380000`, `0x023E0000`) und Boot-Daten der Firmware (`0x023FF800` bis `0x023FFDA8`) |

**Grenzen:** Gelaufen ist nur der Start, ein kleiner Teil der Blöcke, und vermutlich kein Thumb-Code. Die Übersetzung aller anderen Befehle ist kompiliert, aber nicht ausgeführt. Es gibt noch keine Interrupts, keine Timer, keine BIOS-Aufrufe und keinen Dateizugriff; ein `swi` bricht das Programm ab. Der Code ist ohne Optimierung gebaut. **Für Phase 3 nötig:** Firmware-Bootdaten (Name, Sprache, Einstellungen) nachbilden.

### Ergebnis Phase 3 [gemessen, gegen melonDS ohne Eingabe bis Tick 2610]

`tools/recomp.py` übersetzt nun auch alle 108 Overlays (rund 210.000 Blöcke, Hauptprogramm rund 33.500). Zur Laufzeit folgt `native/runtime/overlay.cpp` den Aufrufen von `load_overlay` und `unload_overlay` und prüft vor jedem Overlay-Block den ersten Befehl gegen den RAM. Geladen wurden im Lauf bisher nur die Overlays 83, 84, 85, 86 und 105.

Der ARM7-Ersatz bedient die PXI-Kanäle 5 (Uhr), 6 (Touch), 7 (Sound), 8 (Strom und Soundchip) und 11 (Spielstand). Kanal 11 folgt dem echten ARM7-Code (`arm7.bin` ab Dateioffset 0x8340), die übrigen dem ARM9-Code und Messwerten aus melonDS.

**Erreicht:** Mit Tasten-Skript und Spielstand läuft das Spiel über Titel (Szene 88) und Spielstand-Auswahl (Szene 90) bis ins Hauptmenü (Szene 91). Belegt über die Szenennummern; ein Bild gibt es nativ noch nicht. Welche Szene was zeigt, ist in melonDS am Bild geprüft.

| Vergleich an den Ticks 60, 120, 300, 1000, 2400, 2600 | Ergebnis |
|---|---|
| ITCM, Palette | überall 100 % |
| Hauptspeicher unter `0x02380000` | 99,982 bis 99,994 %, bei Tick 2600 99,543 % |
| DTCM | 99,493 bis 99,915 % |
| OAM | 81,25 bis 100 %, bei Ticks 60, 120 und 2600 gleich |
| Szenenfolge | gleiche Reihenfolge, andere Zeitpunkte: Szene 88 bei Tick 135 (nativ 131), Szene 110 bei Tick 2484 (nativ 2412) |
| Zwei native Läufe mit gleichem Spielstand, ohne Eingabe | bei Tick 2000 in allen fünf Bereichen byteweise gleich |

**Deutung der Reste, nicht einzeln belegt:** Die Zeitverschiebung trifft Animationen, OAM und Heap-Inhalte. Der Sound-Bereich ab `0x02159880` bleibt nativ leer, weil dort der echte ARM7 Zustände einträgt. Der Bereich `0x02162D64` bis `0x02162E20` wird von Code um `0x02079870` benutzt; was dort steht, ist unbekannt. **Die Abnahme ist damit ein enger Abgleich, keine Bytegleichheit.**

**Grenzen:**
- Die Taktung ist ein Platzhalter (`kBlocksPerLine`), die Ursache der Zeitverschiebung ist nicht untersucht.
- Kein Bild, kein Ton. Eingaben nur per Skript (`--input`).
- Der Spielstand liegt nur im Arbeitsspeicher. `--save` liest ihn nur, geschrieben wird nie in eine Datei. Lesen, Schreiben und Löschen sind nicht einzeln getestet.
- Sound-Befehle werden nur abgenommen. Sound-Zustände fehlen, das kann Spielabläufe verändern, die auf das Ende eines Klangs warten.
- Noch nicht aufgerufener Code kann weiter an fehlenden Einsprüngen stoppen; `tools/recomp_loop.ps1` fängt das ab. Thumb-Sprungtabellen werden nicht verfolgt.
- Das Hauptmenü wurde nur im nativen Lauf per Szenennummer erreicht und dort nicht mit melonDS verglichen.
- Die Wiederholbarkeit ist nur auf einem Rechner und in einem Lauf bis Tick 2000 geprüft.

### Ergebnis Phase 4 [gemessen, gegen Fensterbilder aus melonDS]

`MarioPartyDS.exe` zeigt das Spiel in einem SDL3-Fenster (SDL 3.4.20, liegt nur lokal unter `native/third_party/`). Der Spielkern liegt in `native/runtime/machine.cpp`, der 2D-Renderer in `ppu.cpp`.

| Bildvergleich, je alle 98.304 Pixel | Ergebnis |
|---|---|
| Titelbild beim Einfahren (nativ Tick 200) | 100 % gleich |
| Spielstand-Auswahl ohne Zeiger (nativ Tick 895) | 100 % gleich |
| Spielstand-Auswahl mit Zeiger auf 1 (nativ Tick 1333) | 100 % gleich |
| Hauptmenü (nativ Tick 1467) | 100 % gleich |

**Methode:** Die Läufe sind zeitlich versetzt, deshalb sucht `tools/find_matching_frame.py` das passende Bild in einem Bereich nativer Bilder. Geprüft ist damit, dass der Renderer dieses Bild erzeugen kann, nicht dass es zum selben Zeitpunkt erscheint. Nur vier Standbilder, die Bildfolge über die Zeit ist nicht verglichen.

**Über echte Fensterereignisse getestet:** Tastatur auf dem Titel, Mausklick auf den Titel, Anlegen der Spielstand-Datei auf leerem Speicher (8192 Byte, „Data has been created“) und Lesen beim nächsten Start.

**Grenzen:**
- Controller, `F10` und `F11` sind eingebaut, aber ungetestet. Der Controller-Zeiger für den Touchscreen ebenfalls.
- Nicht pixelweise verglichen: Logos, „Touch to Start“, Intro, alle Bildschirme hinter dem Hauptmenü. Viele Renderer-Funktionen (Rotation, Bitmap-Sprites, Fenster, Mosaik) sind damit vermutlich unbelegt.
- Ein Spielprofil anlegen, Fortschritt speichern und wieder laden ist nicht getestet.
- 3D und Display-Capture werden nicht gezeichnet, Ton fehlt. Auf dem Weg ins Hauptmenü kommen 3D und Capture laut Zähler nicht vor.
- Tastenbelegung fest eingebaut, die Einstellungen fehlen noch.
- Tempo knapp: 600 Bilder in rund 11 s samt Start, Soll rund 10 s. Der Spielcode ist ohne Optimierung gebaut.
- Offen: In melonDS erschien nach längerem Warten im Hauptmenü ein Bildschirm „Touch the Screen“ mit Sternenhimmel, und gleichzeitig brach die Stub-Verbindung ab. Ob beides zusammenhängt, ist nicht untersucht.

### Ergebnis Phase 5 [gemessen, gegen ein Fensterbild aus melonDS]

`native/runtime/gpu3d.cpp` enthält Geometrie (Matrizen, Licht, Polygone, Clipping, Tests) als exakte Ganzzahl-Arbeit und einen Rasterizer mit Texturen, Tiefenpuffer, Transparenz, Schatten, Kanten und Nebel. Der Rasterizer rechnet mit Gleitkomma; sein Bild fließt nicht ins Spiel zurück, darf also keine Partie auseinanderlaufen lassen. Display-Capture ist in `ppu.cpp` ergänzt.

**Erreicht:** Mit Tasten-Skript und der gespeicherten Partie läuft nativ eine ganze Runde auf Wiggler's Garden samt Minispiel (Szene 44) und zurück aufs Brett, von Runde 2 auf Runde 3. Am Brett „Go, Mario!“ zeigen beide Programme dieselbe Szene.

| Vergleich Brett, je alle Pixel des jeweiligen Bildschirms | Ergebnis |
|---|---|
| Oberer Bildschirm (2D) | 148 von 49.152 Pixeln abweichend, Stelle: Figuren auf der Karte (Animation) |
| Unterer Bildschirm (3D), exakt gleich | 40,0 % |
| Unterer Bildschirm, höchstens 16 von 255 Abweichung | 91,7 % |
| Unterer Bildschirm, höchstens 64 von 255 Abweichung | 97,0 % |

**Das 3D-Bild ist also ähnlich, aber nicht pixelgleich.** Gezählt wurde nur ein Bild. Die Hardware interpoliert anders als mein Rasterizer, Antialiasing fehlt ganz.

**Tempo:** Brett im Testlauf ohne Fenster rund 80 Bilder pro Sekunde, Soll 60 (110 Mio. Blöcke in 26 s). Im Fenster auf dem Brett nicht gemessen.

**Grenzen:**
- Mit melonDS verglichen ist nur dieses eine Bild. Das Minispiel und die Runde sind nur am eigenen Bild angesehen.
- Nur ein Brett und ein Minispiel gelaufen; von 73 Minispielen ist eines gesehen. Neue Stellen können noch an fehlendem Code stoppen.
- Rein zufällige A-Tasten im Skript: Das Minispiel wurde durchlaufen, nicht sinnvoll gespielt. Toad scheint im Bild die eigene Figur zu sein, das ist nicht weiter geprüft.
- Display-Capture lief 1.152 Zeilen lang, die Wirkung ist nicht gegen melonDS geprüft. Der Hauptspeicher-FIFO als Capture-Quelle fehlt.
- Fehlt: Antialiasing, 1-Punkt-Polygone, Rear-Plane-Bitmap, Drahtgitter bei Alpha 0.
- Die Polygon-Zählung im Spiel konnte nicht verglichen werden: melonDS meldete den Wert mitten im Aufbau einer Liste.

### Ergebnis Phase 6 [Hörtest vom Nutzer, Zähler gegen melonDS]

`native/runtime/sound.cpp` baut den ARM7-Soundtreiber nach: Sequenzer mit Spuren und Variablen, Hüllkurven, Modulation, Portamento, 16 Kanäle mit PCM, ADPCM, PSG und Rauschen, Mixer mit Ausgabe über SDL3. Tabellen werden zur Laufzeit aus dem ARM7-Teil der ROM gelesen, nicht mitgeliefert. Der Spielzustand hängt nur an Ganzzahlen; die Lautstärken rechnen mit Gleitkomma, fließen aber nicht ins Spiel zurück.

| Prüfung | Ergebnis |
|---|---|
| Hörtest durch den Nutzer | „Klingt richtig“; die Änderung an Spur-Parametern und Priorität kam danach und ist noch nicht gehört |
| Player-Tick bei Spiel-Tick 1000 | 642, in melonDS 642 |
| Player-Tick bei Spiel-Tick 300 | 403, in melonDS 397 |
| Beginn Szene 110 | Tick 2480, in melonDS 2484 (vor dem Sound 2412) |
| Zwei gleiche Läufe | bei Tick 1000 im Hauptspeicher byteweise gleich; vor der letzten Änderung geprüft |
| Runde mit Minispiel | läuft durch, 11.533 Noten, keine ungebauten Befehle |

**Grenzen:**
- Die Bedeutung vieler Sequenz-Befehle stammt aus der Kenntnis des Formats, nicht aus jeder einzelnen Stelle im ARM7-Code.
- `0x1E` bis `0x20` als „dieser Speicher wird frei“ sind aus den Argumenten erschlossen. `0x18` wird ignoriert.
- Nicht gebaut: direkte Kanalsteuerung (`0x0C` bis `0x17`), Alarme, Sound-Capture, Mikrofon. Sie werden gezählt und protokolliert, kamen bisher nicht vor. Das Mic-Test-Menü und mikrofongesteuerte Minispiele sind damit offen.
- Die Gesamtlautstärke (Faktor 0,5) ist selbst festgelegt. PSG-Stimmung und Rauschen sind nur nach Formel gebaut.
- Sound-Zustände außer Tick, Player und Kanälen werden nicht geschrieben.

### Ergebnis Phase 7 [gemessen, nativer Lauf mit Tasten-Skript]

**Ganze Party:** Neue Partie auf Wiggler's Garden, Battle Royale, 10 Runden, Mario gegen drei Computer, gesteuert nur durch A und einen Touch alle 45 Ticks. Ohne Abbruch bis „Party Results“ (Szene 95) bei Tick 100.907, rund 28 Minuten Spielzeit. Dazwischen 11 Minispiele (Szenen 48, 49, 46, 47, 55, 41, 20, 53, 57, 45, 61), jedes mit Rückkehr aufs Brett. Der Spielstand lag dabei nur im Speicher.

**Minispiel-Matrix:** `tools/minigame_matrix.py` startet alle 73 Minispiele, je in einem eigenen Prozess, mit gemischten Tasten, Touch und Pusten. Ergebnis in `run/matrix/results.csv` (nicht im Repository, enthält Spieltitel).

| Prüfung | Ergebnis |
|---|---|
| Läuft bis zum Ende der Testzeit (1.500 Ticks) | 73 von 73 |
| Eingestiegen über eine Abkürzung (`--force-mode`) | 69, davon 4 Pustespiele erst nach Einbau des Mikrofons |
| Nur über das echte Menü startbar | 4 Puzzles (Szenen 72, 73, 74, 76): Über die Abkürzung fehlen ihre Textdateien, das Spiel hält in einer eigenen Prüfung an |
| Zeichnet 3D, macht Ton | alle, die 3D brauchen; alle haben Noten |

**Mikrofon:** Kanal 9 nachgebaut nach dem ARM9-Code. Die Eingabe ist nur „pusten oder nicht“, auf der Leertaste oder dem linken Trigger. So kann sie später mit den Tasten übers Netz gehen.

**Grenzen:**
- Die Eingaben zielen auf nichts. „Läuft“ heißt: startet, zeichnet, tickt weiter. Nicht: ist spielbar und richtig. Kein Minispiel ist gezielt gespielt worden.
- Mit melonDS ist von den Minispielen keines verglichen.
- Die Party lief auf einem Brett von fünf, im Modus Battle Royale, ohne Tag Battle und Duel. Story-, Minispiel-, Puzzle-Modus und Galerie sind nur angelaufen oder gar nicht geprüft.
- Nach einer Abkürzung landet das Spiel am Ende eines Minispiels im internen Entwicklermenü „MP-NDS MENU“. Das ist Spielverhalten, kein Fehler: Ohne echtes Menü weiß es nicht, wohin zurück.
- Fehlender Code wird weiter erst beim Ausführen gefunden. Diese Party brauchte rund 25 Nachträge, die Matrix etwa 140.

### Phase 8: Analyse vorab [aus dem Code gelesen]

**Spiel-Schnittstelle zum Netz (Glue, 0x02026B14 bis 0x02028100):** klein und klar.

| Adresse | Aufgabe (aus dem Code) |
|---|---|
| `0x02027110` | Netz an? Liefert WH-Zustand + 1, oder 0 |
| `0x0202713C` | Bin ich Gast? (über `0x02024FA8`) |
| `0x02027158` | Bitmaske der verbundenen Spieler; Host aus WH, Gast aus den geteilten Daten |
| `0x0202719C` | Anzahl der Spieler (Bits der Maske) |
| `0x02027068` | Warten, bis alle Spieler dasselbe Synchronisations-Bit melden |
| `0x02026C80` | Netz einschalten: WM an, `WH_Initialize` (`0x02024954`), Puffer leeren |
| `0x02026D58` | Netz aus |
| `0x02026DDC` | Als Host öffnen: Datenaustausch einrichten (`0x020229E0`), dann `0x02024A9C` (vermutlich `WH_ParentConnect`) |
| `0x02026E80` | Als Gast verbinden (`0x02022BF8`, `0x020238B0`); bei Start per Download mit den Host-Daten ab `0x027FFC44` |

**WH-Bibliothek (0x02022000 bis 0x02024F4C):** nur von der Glue-Schicht und vom Eingabe-Code aufgerufen, rund 20 Einstiege. Zustände wie im SDK-Beispiel (`0x020245B0` liefert sie): 1 bereit, 2 Suche, 3 beschäftigt, 7 Kanalmessung, 8 Verbindung fehlgeschlagen, 9 Fehler.

**Start als Download-Gast** (Bootart 2, neue Option `--boot-type 2`): Das Spiel bleibt in Szene 0 und wartet auf PXI-Kanal 15. Der gehört vermutlich zum Funktreiber des ARM7. Gäste laden im Original Overlays und Dateien über Funk vom Host (WFS, Prüfsummen ab `0x020BD408`). 27 Stellen im Code unterscheiden die Bootart.

**Entschieden: Weg 2** (WH, MB und WFS auf Ebene ihrer Funktionen ersetzen; Gäste lesen aus der eigenen ROM). Weg 1 wäre der ARM7-Funktreiber gewesen.

### Ergebnis Phase 8 [gemessen, 4 Prozesse auf einem PC, TCP über localhost]

**Aufbau (`native/runtime/net.cpp`):** Eingriffe an Funktionsanfängen des Spiels, kein geänderter Spielcode.

| Baustein | Ersetzt durch |
|---|---|
| MB (Download Play, Host) | Ereignisse 2, 10, 14, 7, 9, 12 an den spieleigenen Handler `0x02017920`, je Gast nacheinander |
| WH-Zustand, Verbinden, Ende | eigene Zustände in `0x020D1AE0`; Gast wartet in `0x020238B0`, bis der Host ihn aufnimmt |
| Datenaustausch `0x02024D44` | Lockstep: Gäste senden ihren Datensatz (0x26 Bytes), der Host verteilt alle vier; alle warten auf jeden Schritt |
| Port-Daten `0x02024410` / Empfänger `0x02024CC4` | Nachricht an die anderen; Zustellung an den Empfänger des Spiels im nächsten Tick, Sende-Rückmeldung ebenso |
| WFS | Stubs; der Gast startet sein Dateisystem wie von Karte (Bootart nur dafür 1, Kartenzugriff `0x0215B540` an) |
| Download-Startdaten | Host schickt beim Beitritt die Sprache; der Gast legt Host-BssDesc (`0x027FFC42`, GGID `0x390`) und Parameter (`0x027FFBE0`) ab |

**Belegt:**

| Prüfung | Ergebnis |
|---|---|
| Lobby | Host sieht Guest1 bis Guest3 in „Find Players“, Gäste zeigen „You are P2/P3/P4“ |
| Menüs | Gäste folgen der Auswahl des Hosts („P1 is making selections“); „Duel Battle“ bei 4 Spielern gesperrt |
| Figurenwahl | jeder wählt selbst (Mario, Luigi, Peach, Yoshi), auf allen vier gleich |
| Gleichschritt | Szenenwechsel bei allen beim selben Schritt; drei gleiche Läufe mit gleicher Prüfsumme |
| Party | Wiggler's Garden, Battle Royale, 10 Runden: bis „Party Results“ (Szene 95) bei Host-Tick 172.811, 178.774 Schritte, 13 Minispiele, ohne Abbruch, 46 min Laufzeit |
| Endstand | auf allen vier gleich: Peach (Guest2) 1 Stern 19 Münzen, Yoshi 47, Mario 36, Luigi 7 |
| Bild an gleichen Schritten | Unterschiede nur bei eigener Ansicht (eigenes Auto, eigene Markierung, Speicherhinweis des Hosts); Ergebnistafeln gleich |
| Hauptspeicher an gleichen Schritten | 9.200 bis 11.200 von 4 MB Bytes verschieden, über die Party ohne Anstieg |

**Werkzeuge:** `--net-host PORT`, `--net-guest PORT`, `--boot-type 2`, `--dump-step N`, `tools/compare_net_dumps.py`. Ein Spieler, der 10 s keinen Schritt macht, meldet seine Stelle („STALL“).

**Grenzen:**
- Nur TCP auf einem Rechner. Kein Internet, keine Verzögerung, kein Paketverlust, kein Wiederverbinden (Phasen 9 bis 11).
- Die verschiedenen Bytes im Hauptspeicher sind nicht einzeln zugeordnet. Münzen und Sterne werden noch nicht aus dem Speicher verglichen, nur über die Ergebnistafeln im Bild.
- Zweimal blieben Gäste früh über 20 s stehen; danach nicht wieder auslösbar. Ursache offen.
- Eingaben nur gescriptet (A und Touch alle 45 Ticks). Minispiele wurden nicht gezielt gespielt.
- Gast-Spielernummern entstehen aus der Reihenfolge des Verbindens und sind nicht fest.

### Ergebnis Phase 9 [gemessen, 4 bzw. 2 Prozesse auf einem PC, TCP über localhost]

**Aufbau (`tools/net_minigame_matrix.py`):** Je Minispiel ein Host und drei Gäste als eigene Prozesse, jedes Spiel mit eigenem Port. Der Host geht über das echte Menü (Free Play, Tab und Eintrag), die Gäste wählen Luigi, Peach und Yoshi. Puzzles laufen über den Puzzle-Modus mit einem Gast. Danach drücken alle Spieler gescriptet Tasten und tippen auf den Touchscreen, jeder mit anderem Muster. Fehlender Code wird in Runden nachgetragen, bis kein Spiel mehr welchen meldet. Ergebnis in `run/netmatrix_all.csv` (nicht im Repository, enthält Spieltitel).

„Gleich“ heißt hier: Alle Spieler betreten das Minispiel im selben Austauschschritt und verlassen es im selben Schritt, keiner bricht ab.

| Gruppe | Zeilen | Gleich bei allen |
|---|---|---|
| 4-Player (Free Play) | 32 | 32 |
| 1-vs-3 | 12 | 12 |
| 2-vs-2 | 13 | 13 |
| Battle | 5 | 5 |
| Puzzles (2 Spieler) | 5 | 5 |
| Bosse (501 bis 505) | 5 | nicht angeboten |
| Triangle Twisters (606) | 1 | gesperrt |
| **Summe** | **73** | **67 von 67 angebotenen** |

**Weitere Befunde:**
- Spieldauer 1.071 bis 19.174 Schritte, Median 3.045.
- Endbilder: 40 Spiele pixelgleich bei allen. Bei den übrigen zeigt jeder Spieler seine eigene Ansicht: eigene Farbe, eigenes Fahrzeug, vertauschte Bildschirme beim Einzelspieler in 1-vs-3. Bei 9 Spielen habe ich die Bilder angesehen. Spielstand, Zeiten und Siegertext waren gleich.
- 24 Codestellen kamen nach, vor allem aus Overlay 62.

**Fehlschläge, alle im Testaufbau, nicht im Spiel:**
- Gleiche Ports bei gleichzeitigen Läufen.
- Zu knappes Rechenbudget für den Host.
- Zu kurze Spielzeit: Mario's Puzzle Party endet erst, wenn ein Stapel voll ist, also nur solange gedrückt wird.

Nach den Korrekturen liefen die Spiele beim nächsten Lauf durch.

**Grenzen:**
- Bosse kommen in keinem Mehrspielermenü vor. Triangle Twisters erscheint als „??????“ und braucht einen Fortschritt, den der Testspielstand nicht hat.
- Die Free-Play-Spiele sind nur über `--unlock-minigames` (Hilfsmittel im Test, setzt die Bits ab `0x021547B8`) alle wählbar. Ohne das zeigt der Spielstand nur die gespielten.
- Eingaben gescriptet, nichts gezielt gespielt. Ob jedes Spiel mit echten Spielern richtig zu bedienen ist, ist nicht geprüft. Pustespiele haben im Netz nur „pusten oder nicht“.
- Gleichheit nur über Schritte und Bilder. Der Hauptspeicher wurde nicht je Spiel verglichen.
- Weiter nur ein Rechner, kein echtes Netz.

### Ergebnis Phase 10 [gemessen, Server und 4 Prozesse auf einem PC; Fenster mit echten Maus- und Tastaturereignissen]

**Aufbau:**

| Teil | Datei | Aufgabe |
|---|---|---|
| Server | `native/server/server.cpp` → `MarioPartyDS-server.exe` | Lobby und Codes; nach dem Start Weiterleitung des Spielverkehrs. Enthält keinen Spielcode und keine Spielinhalte |
| Protokoll | `native/runtime/lobby_protocol.h` | TCP, Nachrichten mit Länge und Typ, Zahlen little endian |
| Client | `native/runtime/lobby.cpp` | Verbinden, Raum erstellen und beitreten, Figur, Bereit, Start, Ping jede Sekunde |
| Weiterleitung | `native/runtime/net.cpp` | Der Host hat eine Verbindung zum Server für alle Gäste. Spielernummer = Platz in der Lobby |
| Figurenwahl | `native/runtime/net.cpp` | Die Lobby-Figur wird auf dem Figurenbildschirm des Spiels getippt. Die Auswahllogik bleibt die des Spiels |
| Oberfläche | `native/src/launcher.cpp` | Hauptmenü, Online, Beitreten, Raum, Einstellungen; Esc im Spiel fragt „Zurück zum Hauptmenü?“ |

**Codes:** 6 Zeichen aus `A–Z` und `2–9` ohne `I`, `O`, `0` und `1`. Nur der Server vergibt sie. Ein Code verfällt nach 30 Minuten ohne Start (`--code-lifetime`) oder wenn der Host geht. Verbrauchte Codes werden nicht neu vergeben und als „abgelaufen“ gemeldet.

**Figurenbildschirm erkennen:** Das Spiel holt den Hinweis „Select a character.“ (Host) oder „P2/P3/P4, select a character.“ über `get_message` (`0x020187F4`). Die Text-IDs stammen aus der Pfadtabelle `0x02098BA0`:

| Datei | Host | P2 |
|---|---|---|
| `0x04` (Party) | `0x0F` | `0x11` |
| `0x17` (mgbridge) | `0x1E` | `0x4C` |
| `0x19` (Free Play) | `0x01` | `0x08` |
| `0x1A`, `0x1B`, `0x1C` | `0x00` | `0x24`, `0x23`, `0x2E` |
| `0x1E` (Puzzle) | `0x01` | `0x0B` |
| `0x20` (Extra) | `0x12` | `0x13` |

60 Ticks nach dem Hinweis tippt das Programm die Figur an. Die Knöpfe liegen in zwei Spalten: x 75 und x 184, y 36 + 30 × Zeile.

**Belegt:**

| Prüfung | Ergebnis |
|---|---|
| Regeln des Servers (`tools/lobby_test.py --rules`, Protokoll direkt) | 16 von 16: Codeformat, kein doppelter Code bei 21 Räumen, vier Spieler sehen einander, Code in Kleinbuchstaben, unbekannter Code, fünfter Spieler, belegte Figur beim Beitritt und beim Wechsel, Start nur durch den Host und nur wenn alle bereit sind, Verlassen und Verbindungsabbruch aktualisieren die anderen, Host geht = Raum zu, gestarteter Raum nimmt niemanden auf, Weiterleitung in beide Richtungen und Meldung eines weggefallenen Gastes, falsche Version, ungültige Namen, kaputte Nachrichten (der Server läuft danach weiter), abgelaufener Code |
| Echter Client abgewiesen | `--join ABCDEF` endet mit Code 3 und „Diesen Code gibt es nicht.“ |
| Raum bis Spiel, 4 Prozesse (`--game`) | Host erstellt, 3 Gäste treten mit dem Code bei, Start; Spieler 1 bis 4 in Beitrittsreihenfolge. Jeder tippt seine Figur selbst (Mario, Luigi, Peach, Yoshi), alle bei Schritt 1609; auf dem Bild des Hosts stehen P1 bis P4 mit diesen Figuren. Minispiel (Szene 1) bei allen von Schritt 3759 bis 5785 |
| Fenster als Host | Hauptmenü → Online → Raum erstellen → Code erscheint; Gast tritt bei und erscheint; Figur mit Pfeiltaste gewechselt (Peach → Daisy), beim Gast sichtbar; Spiel starten → beide booten, Titel „online, P1 von 2“ |
| Fenster als Gast | Einstellungen sichtbar; Code getippt (mit Korrektur per Rücktaste), Beitreten, Bereit; der Host startet; das Fenster zeigt beim Warten „Warte auf die anderen Spieler …“ und reagiert weiter |

**Abweichungen von der Architektur:** Lobby über einfaches TCP mit eigenem Binärformat, nicht WebSocket und MessagePack. Der Server ist für den Start ein Vermittler, autoritativ ist weiterhin die Host-Instanz des Spiels. TLS fehlt (Phase 12).

**Grenzen:**
- Nur auf einem PC getestet (`--bind 127.0.0.1`). Der Server wartet sonst auf allen Adressen. Windows fragt dann einmal nach einer Firewall-Freigabe; die Entscheidung bleibt beim Nutzer.
- Die automatische Figurenwahl ist nur in Free Play geprüft. Die übrigen Modi sind aus den Texten abgeleitet; die Lage der Knöpfe dort ist nicht geprüft.
- Wer das Spiel verlässt oder die Verbindung verliert, beendet die Partie für alle (Phase 11; seitdem: Verbindungsabbrüche überbrückt, absichtliches Verlassen beendet weiterhin).
- Die Oberfläche nutzt die eingebaute SDL-Schrift (ASCII; Umlaute als Punkte über dem Buchstaben, ß als „ss“) und die Farben der Spielmenüs, nicht die Schrift des Spiels.
- „Zurück zum Hauptmenü“ startet das Programm neu, weil die Maschine nur einmal je Prozess bootet.
- Gamepad im Menü: Steuerkreuz, A und B, ohne Test mit echtem Controller.

### Ergebnis Phase 11 [gemessen, Server und 4 Prozesse auf einem PC, je Spieler ein Netzsimulator]

**Aufbau:**

| Teil | Datei | Aufgabe |
|---|---|---|
| Nummerierte Nachrichten | `native/runtime/net.cpp` | Jede Spielnachricht trägt eine Nummer und die letzte empfangene Nummer der Gegenseite. Ein Gast hält seine unbestätigten Nachrichten, der Host alle gesendeten. Nach einem Abbruch tauschen beide aus, was sie haben (`kSync`), und senden den Rest erneut, in Stücken von höchstens 256 KB je Durchlauf |
| Wiederverbinden | `net.cpp`, `server.cpp` | Ein Gast verbindet sich jede Sekunde neu, ein Host über den Server ebenso. Beim Start gibt der Server jedem Platz einen Schlüssel (64 Bit). Mit `kResume` und diesem Schlüssel kommt man auf seinen Platz zurück. Ein gestarteter Raum bleibt bestehen, bis der Host ihn absichtlich verlässt oder 10 Minuten lang niemand verbunden ist |
| Serverneustart | `server.cpp` | Der Server speichert keinen Spielzustand. Nach einem Neustart baut der Host den Raum mit Code und allen Schlüsseln wieder auf; Gäste, die vor ihm kommen, hören „noch einmal versuchen“ |
| Tote Leitung | `net.cpp`, `server.cpp` | Ping jede Sekunde. Kommt 6 s lang nichts, gilt die Verbindung als tot und wird neu aufgebaut; der Server trennt nach 20 s Stille |
| Eingabeverzögerung | `net.cpp` | Der Datensatz des N-ten Bildes eines Spielers kommt in seinen N-ten Schritt. Mit Verzögerung D haben die ersten D Bilder des Austauschs keinen Schritt. Das Spiel lässt dann seinen Spieltakt aus (`0x01FFC4D4`), wie beim Start des Funk-Datenaustauschs. Danach sind D Datensätze unterwegs, und das Warten auf die anderen entfällt, solange der Weg Host–Gast–Host kürzer ist als D Bilder |
| Wahl der Verzögerung | `server.cpp` | Beim Start: 1 + Ping des Hosts plus größter Ping eines Gastes, in Bildern zu 1/60 s, höchstens 15 (`--input-delay` legt sie fest). Ping ist das kleinste der letzten fünf Lobby-Pings |
| Port-Daten | `net.cpp` | Daten vom Host tragen seine Schrittnummer. Der Gast übergibt sie nach dem Schritt, der auf das Senden folgt. So ist der Zeitpunkt unabhängig vom Netz |
| Ende der Partie | `net.cpp` | Wer absichtlich geht (Esc → Hauptmenü, Fenster schließen), beendet die Partie für alle. Wer länger als 120 s weg ist, ebenso. Alle sehen einen Satz dazu und kommen ins Hauptmenü zurück |
| Neuer Programmstart eines Gastes | `net.cpp`, `launcher.cpp` | Ein Gast schreibt beim Start Raum, Platz und Schlüssel in eine Datei (`rejoin.txt`). Endet sein Programm ohne Abmelden (Absturz, Abschuss), bietet das Hauptmenü „Zurück in die Partie“. Der neue Prozess bootet, der Host sendet ihm alle bisherigen Nachrichten, und er spielt die Partie bis zum Stand der anderen nach. Was er dabei selbst erzeugt und der Host schon hat, sendet er nicht noch einmal. Die anderen warten so lange |
| Anzeige | `main.cpp` | Fenstertitel jede Sekunde, z. B. „online, P1 von 4, Ping P2 63 ms, P3 63 ms, P4 63 ms“. Beim Warten der Grund mit Restzeit, z. B. „P3 (Name): Verbindung unterbrochen / Warte auf die Rückkehr ... noch 97 s“. Beim Nachholen „Hole die laufende Partie nach ... Schritt X von Y“, ohne Bild und Bildtakt |

**Netzsimulator:** `tools/netsim.py`, ein TCP-Zwischenstück je Spieler zwischen Spiel und Server. Es verzögert jedes Stück um eine feste Zeit plus Zufall und hält die Reihenfolge ein. „Verlust“ heißt: ein Stück kommt 200 ms später, so wie TCP es nach einem verlorenen Segment erneut sendet. Über eine Steuerdatei: `cut S` trennt alle Verbindungen und weist S Sekunden lang neue ab; `silent S` leitet S Sekunden lang nichts weiter, lässt die Verbindungen aber offen. Zeitauflösung von asyncio unter Windows: etwa 15 ms.

**Belegt** (`tools/netsim_test.py`, alle 12 Fälle, ein Lauf; Server, 4 Prozesse, je ein Simulator; Free Play bis zum Ende des ersten Minispiels):

| Fall | Netz je Spieler (eine Richtung) | Ergebnis |
|---|---|---|
| plain | keine Verzögerung | Verzögerung 3, Ping Gast–Host 1–31 ms, Minispiel bei allen von Schritt 3772 bis 5816, 70 s |
| rtt50 | 25 ms + bis 5 ms | Verzögerung 12, Ping Gast–Host 119–143 ms, Minispiel bei allen gleich, 87 s |
| rtt150 | 75 ms + bis 15 ms | Verzögerung 15, Ping 319–390 ms, gleich, 163 s |
| rtt300_loss | 150 ms + bis 25 ms, 2 % Verlust | Verzögerung 15, Ping 629–1074 ms, gleich, 339 s |
| guest_cut | Gast P3 4 s getrennt | P3 verbindet sich 5-mal neu, Host sendet 7 Nachrichten erneut, gleich |
| guest_silent | P2 10 s ohne Daten bei offener Verbindung | nach 6 s erkannt, neu verbunden, gleich |
| host_cut | Host 4 s getrennt | Host verbindet sich neu, alle Seiten senden Fehlendes, gleich |
| server_restart | Server beendet, nach 3 s neu gestartet | alle verbinden sich 2–3-mal neu, der Host baut den Raum wieder auf, gleich |
| guest_restart | Prozess von P3 beendet, nach 2 s neu mit `--rejoin` | Host sendet 4293 Nachrichten, der neue Prozess holt mit etwa 133 Schritten/s auf, Minispiel bei allen von Schritt 3791 bis 5848 |
| guest_gone | Prozess von P4 beendet, kein neuer | nach der Wartezeit (im Test 20 s) enden die anderen drei mit „P4 (Guest3) ist nicht zurückgekommen. Die Partie ist beendet.“ |
| guest_leaves | P4 geht absichtlich | die anderen enden mit „P4 (Guest3) hat das Spiel verlassen …“ |
| host_leaves | Host geht absichtlich | alle Gäste enden mit „Der Host hat das Spiel beendet.“ |

„Gleich“ heißt: alle vier spielen das Minispiel vom selben Schritt bis zum selben Schritt und beenden es. Dazu kommen die Regeln des Servers (`tools/lobby_test.py --rules`): 25 von 25, neu sind Startdaten mit Verzögerung und Schlüsseln, Ping in der Weiterleitung, Rückkehr mit Schlüssel, falscher Schlüssel, Ersatz einer noch nicht erkannten toten Verbindung, Host weg mit Raum erhalten, Host verlässt einen gestarteten Raum, Serverneustart mit Wiederaufbau und Trennung stiller Verbindungen. Der Direktmodus ohne Server läuft weiter (`net_minigame_matrix.py --scenes 7`).

**Unterwegs gefunden:**
- Die Warteschleife wartete erst bis zu 10 ms auf Daten und prüfte danach, ob der nächste Schritt schon da war. Das bremste jeden Schritt. Behoben; die Fälle laufen jetzt 10 bis 40 % schneller, das Nachholen beim neuen Programmstart dreimal so schnell.
- Beim Schließen einer Verbindung gingen Nachrichten verloren, die kurz davor gekommen waren, darunter das Spielende des Hosts. Behoben.

**Abweichungen von der Architektur:** Wiedereintritt ohne Zustandsübertragung: Der Host sendet die bisherigen Nachrichten, und der neue Prozess rechnet die Partie nach. Der Server hält keinen Zustand und muss nichts speichern; nach einem Neustart setzen die Spiele die Partie selbst fort. Weiterhin TCP, kein UDP mit ENet.

**Grenzen:**
- Nur auf einem PC mit dem eigenen Simulator getestet, nicht über das Internet.
- Ab etwa 250 ms Weg Gast–Host reicht die Verzögerung von höchstens 15 Bildern nicht mehr. Das Spiel läuft dann langsamer: bei 340 ms etwa mit 70 %, bei 700 ms bis 1 s etwa mit 30 % der normalen Geschwindigkeit. Jede Eingabe wirkt erst 250 ms später.
- Die Verzögerung wird einmal beim Start gewählt und passt sich danach nicht an.
- Den Ping im Spiel misst nur, wer gerade Nachrichten liest; das geschieht einmal je Bild. Er kann daher bis zu 17 ms zu hoch sein.
- Nachholen nach einem neuen Programmstart: etwa 133 Schritte/s, also gut doppelt so schnell wie das Spiel. Nach 40 Minuten Party wären das etwa 18 Minuten Warten für alle. Der Host bewahrt alle Nachrichten an jeden Gast auf, etwa 200 Byte je Schritt, also etwa 35 MB je Gast nach einer langen Party.
- Der Host kann nicht zurückkehren. Endet sein Programm, warten die Gäste die Wartezeit ab und enden dann.
- Fenster schließen gilt als absichtliches Verlassen und beendet die Partie für alle. Nur ein Programmende ohne Abmelden lässt die Rückkehr zu.
- Die Fensterseite (Titel mit Ping, Wartegrund, Endmeldung, „Zurück in die Partie“, Nachholen ohne Bild) ist gebaut, aber nicht mit echten Eingaben getestet: Beim Testversuch lag zweimal ein anderes Fenster vorn, und das Testskript sendet dann nichts. Belegt sind dieselben Abläufe nur im Prozess ohne Fenster.
- Wer den Code kennt, kann nach einem Serverneustart einen Raum mit diesem Code anlegen, bevor der echte Host zurück ist. Der echte Host wird dann abgewiesen.

**Parallel möglich:** 2D und Sound nach Phase 3. Lobby-Server nach Phase 0, unabhängig vom Spiel.

**Früher Teilnutzen:** Nach Phase 8 ist ein Vier-Spieler-Betrieb im Heimnetz testbar, aber nur nach Bestehen der Tests.

## 9. Tests

| Test | Methode |
|---|---|
| Vier Spieler gleichzeitig, 4 Prozesse | Client ohne Fenster, gescriptete Eingaben, ein Testlauf startet 1 Server und 4 Clients |
| Lobby erstellen und beitreten | Automatisiert, auch ungültige und abgelaufene Codes, fünfter Spieler |
| Würfel, Münzen, Sterne, Züge | Zustand aller Prozesse je Bild per Prüfsumme gleich; Werte aus dem Sim-Speicher, sobald ihre Adressen bekannt sind |
| Minispiele | Pro Spiel Start, Ablauf mit Skript, gleiche Ergebnisse bei allen |
| Abbruch und Wiedereintritt | Prozess beenden, neu verbinden, Prüfsumme wieder gleich |
| Verzögerung | Eigener Proxy (`tools/netsim.py`, TCP, weil das Spiel TCP nutzt): 0, 50, 150, 300 ms, Jitter, späte Pakete als Verlust |
| Betrug | Manipulierter Client sendet ungültige Daten; Server lehnt ab, Zustand unverändert |
| Treue zum Original | Rekompilat gegen melonDS: RAM und Bild |

## 10. Risiken

| Risiko | Folge | Gegenmaßnahme |
|---|---|---|
| Funkmodus deckt wenig ab | N2 nötig, deutlich mehr Eingriffe je Minispiel | Phase 1 zuerst |
| Unentdeckter Code | Abbruch mitten im Spiel | Mitschnitte, Abdeckungs-Prüfung, klare Fehlermeldung |
| 3D weicht ab | Sichtbare Fehler | Vergleich mit Emulator; einzeln prüfbar |
| Determinismus bricht | Zustände laufen auseinander | Keine Fließkomma- oder Zeitabhängigkeit in der Sim; Prüfsummen ab Tag 1 |
| Aufwand übersteigt die Bereitschaft | Abbruch ohne Ergebnis | Jede Phase liefert etwas Nutzbares |
| Rechtliche Lage bei Weitergabe | Exe nicht verteilbar | Entscheidung vor Phase 12 |

## 11. Offene Entscheidungen

1. Architektur B mit N1: **entschieden** (Phase 1).
2. Server: **entschieden am 05.11.2026: auf dem PC des Nutzers.** Folgen: Der Server läuft nur, solange dieser PC an ist. Die anderen erreichen ihn über die öffentliche Adresse des Anschlusses; dafür braucht es eine Portweiterleitung im Router (UDP), oder UPnP, falls der Router es erlaubt. Bei wechselnder öffentlicher Adresse braucht es einen dynamischen DNS-Namen oder einen neuen Einladungscode. Der Lobby-Dienst kann im selben Programm laufen.
3. Weitergabe: Wie kommen die anderen drei Spieler an die Exe, und hat jeder eine eigene ROM? **Offen.**
4. Git-Repository: **angelegt**, nur lokal.
