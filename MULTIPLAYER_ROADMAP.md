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

Fehlgeschlagen: Das Erzwingen eines Szenenwechsels durch Löschen des Task-Flags ließ das Spiel hängen. Tasten-Eingabe über den Emulator: `tools/press_keys.py` setzt bei `0x01FFC3B8` die Tastenmaske in `r9`; noch ungetestet.

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

**Noch nicht belegt:**
- Dass die Instanzen wirklich dieselbe Logik rechnen. Der Code zeigt den Mechanismus, gesehen haben wir ihn nicht laufen. Zufallswerte müssen dabei gleich starten; das ist nicht untersucht.
- Der Umfang. „Fragt den Zustand ab“ bedeutet nicht „läuft im Funkmodus“; auch Story-Menü und Galerie fragen ihn ab. Nicht abfragende Minispiele können den Zustand über Hilfsfunktionen im Hauptprogramm erhalten.
- Ob 2 melonDS-Instanzen das testen können. Der Empfängerstart kommt aus der DS-Firmware, die hier nicht vorliegt.

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
| 0 | Git-Repository (ROM und Spielinhalte ausgeschlossen), CMake, SDL3, Tasten-Eingabe im Emulator | Leeres Fenster als Exe; Tasten wirken im Spiel |
| 1 | **Funkmodus untersuchen:** Umfang, Datenfluss, wer rechnet was; zwei melonDS-Instanzen im lokalen Funkmodus | Liste der Bretter und Minispiele im Funkmodus; Entscheidung N1 oder N2 |
| 2 | Rekompilierer und minimale Laufzeit, Vergleich gegen melonDS | Start bis `main`: Register und RAM gleich |
| 3 | Code-Abdeckung, Interrupts, Task-System, Overlays | Headless bis ins Hauptmenü, RAM-Abgleich über Hunderte Bilder |
| 4 | 2D-Darstellung, Eingabe, Dateien, Spielstand | Titel und Menüs bedienbar; pixelweiser Vergleich |
| 5 | 3D | Brett und Figuren wie im Emulator |
| 6 | Sound | Musik und Effekte im Vergleich hörbar |
| 7 | Lokale Vollständigkeit | Eine Party lokal von Anfang bis Ende; Minispiel-Matrix lokal |
| 8 | Netz im eigenen Rechner: Lockstep, Server headless | 4 Prozesse lokal, Prüfsummen gleich über eine ganze Partie |
| 9 | Minispiele einzeln online, Matrix mit 73 Zeilen | Jedes Spiel gestartet, gespielt, Ergebnis gleich bei allen; Grenzen dokumentiert |
| 10 | Lobby, Codes, Charakterwahl, Oberfläche | Ablauf von Raum bis Start in 4 Prozessen |
| 11 | Abbrüche, Wiedereintritt, Serverneustart, Verzögerung, Ping | Netzsimulator-Tests bestanden |
| 12 | Internet: Server betreiben, Verschlüsselung, Einstellungen, Vollbild | 4 echte PCs an verschiedenen Standorten spielen eine Party |

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
| Verzögerung | Eigener UDP-Proxy: 0, 50, 150, 300 ms, Jitter, Paketverlust |
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

1. Architektur B mit N1 oder N2 nach Phase 1 freigeben.
2. Server: eigener Dauerbetrieb, Miet-Server oder Host-PC.
3. Weitergabe: Wie kommen die anderen drei Spieler an die Exe, und hat jeder eine eigene ROM?
4. Git-Repository anlegen (bisher keines vorhanden).
