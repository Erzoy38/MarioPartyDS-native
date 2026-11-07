# MarioPartyDS-native

**Mario Party DS als natives Windows-Programm, mit Online-Multiplayer für bis zu vier Spieler.**

Der ARM-Code des Spiels wird automatisch nach C++ übersetzt (statische Rekompilierung). Eine eigene Laufzeit ersetzt die Hardware des Nintendo DS: CPU, Speicher, 2D- und 3D-Grafik, Sound, Touchscreen, Mikrofon und Spielstand. Die Originallogik bleibt dabei erhalten, Bretter und Minispiele müssen nicht neu geschrieben werden. Den Funkmodus des DS ersetzt eine verschlüsselte Internet-Verbindung über einen eigenen Server.

> ⚠️ **Dieses Repository enthält kein Spiel.** Keine ROM, keine Grafiken, keine Musik und keinen übersetzten Spielcode. Wer es benutzen will, braucht eine eigene, rechtmäßig erworbene Kopie von *Mario Party DS* (Europa, Rev 1). „Mario Party DS“ und alles, was dazugehört, gehört den jeweiligen Rechteinhabern. Das Projekt steht in keiner Verbindung zu Nintendo oder Hudson Soft.

---

## Stand

Das Projekt wird aktiv entwickelt. Die Einzelheiten mit Messwerten stehen in [`MULTIPLAYER_ROADMAP.md`](MULTIPLAYER_ROADMAP.md).

| Bereich | Stand |
|---|---|
| Rekompilierer und Laufzeit | ✅ Start bis ins Hauptmenü, Register und RAM mit melonDS abgeglichen |
| 2D-Grafik | ✅ Titel und Menüs pixelgleich mit melonDS |
| 3D-Grafik | 🚧 Bretter und Figuren werden gezeichnet, kleine Abweichungen zum Emulator |
| Sound | 🚧 ARM7-Soundtreiber nachgebaut: Musik und Effekte über SDL3 |
| Lokale Party | ✅ Eine ganze Party (10 Runden, 11 Minispiele) lief ohne Abbruch durch |
| Minispiele | 🚧 Alle 73 starten und laufen, gezielt durchgespielt ist noch keins |
| Online: Lobby, Räume, Charakterwahl | ✅ Getestet mit vier Prozessen auf einem PC |
| Online: Abbrüche, Wiedereintritt, Verzögerung | 🚧 Netzsimulator-Tests bestanden |
| Online über das Internet | 🚧 Verschlüsselung und Server fertig, ein Test mit vier PCs an verschiedenen Orten steht noch aus |

✅ funktioniert · 🚧 funktioniert mit Einschränkungen oder ist noch nicht vollständig getestet

## Funktionen

- Läuft nativ unter Windows, ohne Emulator
- Startbildschirm mit ROM-Auswahl, Einstellungen für Sprache, Fenstergröße und Vollbild
- Tastatur, Maus als Touchscreen und Controller-Unterstützung
- Online-Partien für bis zu vier Spieler, mit 6-stelligem Raumcode
- Eigener Server, den der Host direkt aus dem Spiel starten kann
- Verschlüsselte Verbindung (ECDH P-256, AES-256-GCM) mit Server-Fingerabdruck, ohne zusätzliche Bibliotheken
- Kurze Verbindungsabbrüche werden überbrückt, und ein Gast kommt nach einem Absturz wieder in die laufende Partie

## Voraussetzungen

- Windows 10 oder 11
- Die eigene ROM: `Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).nds`
- Python 3
- Visual Studio Build Tools mit C++ (CMake und Ninja sind dabei)
- [SDL3](https://github.com/libsdl-org/SDL/releases): das entpackte `SDL3-devel-*-VC.zip` unter `native/third_party/SDL3`

## Bauen

Die übersetzten Spieldateien werden auf dem eigenen Rechner aus der eigenen ROM erzeugt und nie hochgeladen.

```powershell
# 1. ROM einlesen und entpacken (schreibt nach unpacked/)
python tools/nds_header.py "<pfad zur rom>.nds"
python tools/decompress.py "<pfad zur rom>.nds"

# 2. Spielcode nach C++ übersetzen (schreibt nach native/generated/)
python tools/recomp.py

# 3. Bauen, danach läuft automatisch ein kurzer Start-Test
powershell -ExecutionPolicy Bypass -File native\build.ps1
```

Das Ergebnis liegt in `native\build\`:

| Datei | Zweck |
|---|---|
| `MarioPartyDS.exe` | Das Spiel mit Fenster, Ton und Eingabe |
| `MarioPartyDS-server.exe` | Server für Online-Partien, enthält keinen Spielcode |
| `MarioPartyDS-headless.exe` | Ohne Fenster, für Tests und den Abgleich mit melonDS |

## Spielen

1. `MarioPartyDS.exe` starten.
2. Im Startbildschirm **SELECT ROM IMAGE** wählen und die ROM-Datei angeben. Ab dann heißt der Punkt **START GAME**.
3. Unter **SETTINGS** lassen sich Sprache, Fenstergröße, Vollbild, der eigene Name und die Server-Adresse einstellen.

Spielstand, Einstellungen und Protokoll liegen unter `%APPDATA%\MarioPartyDS\MarioPartyDS\`.

### Steuerung

| DS | Tastatur | Controller |
|---|---|---|
| Steuerkreuz | Pfeiltasten | Steuerkreuz |
| A / B | X / Z | A (rechts) / B (unten) |
| X / Y | S / A | X (oben) / Y (links) |
| L / R | Q / W | Schultertasten |
| Start / Select | Enter / Umschalt rechts | Start / Back |
| Touchscreen | Maus (linke Taste) | Rechter Stick bewegt den Zeiger, rechter Trigger tippt |
| Ins Mikrofon pusten | Leertaste | Linker Trigger |

Dazu kommen **F10** für die Fenstergröße, **F11** für Vollbild und **Esc**, um zum Startbildschirm zurückzukehren.

## Online spielen

Ein Spieler ist der **Host**: Er startet im Spiel unter **PLAY ONLINE → Server hier starten** den Server und erstellt einen Raum. Die anderen tragen seine Adresse ein und treten mit dem Raumcode bei. Der Server läuft standardmäßig auf **TCP-Port 47000**. Spieler außerhalb des Heimnetzes brauchen dafür eine Portweiterleitung am Router des Hosts.

Die vollständige Anleitung mit Firewall, Router, Fingerabdruck und dem Verhalten bei Abbrüchen steht in [`ONLINE.md`](ONLINE.md).

## Aufbau des Repositorys

| Pfad | Inhalt |
|---|---|
| `native/runtime/` | Laufzeit: CPU, Speicher, Hardware, 2D (`ppu`), 3D (`gpu3d`), Sound, Netz, Lobby, Verschlüsselung |
| `native/src/` | Fensterprogramm: Startbildschirm, Menüs, Eingabe |
| `native/server/` | Lobby- und Relay-Server |
| `tools/` | Python-Werkzeuge: Rekompilierer, Disassembler, Entpacker, Abgleich mit melonDS, Netz- und Minispiel-Tests |
| `REVERSE.md` | Notizen zum Reverse Engineering: Speicherbelegung, Overlays, Dateiformate |
| `MULTIPLAYER_ROADMAP.md` | Architektur, Phasen und Messergebnisse |
| `ONLINE.md` | Anleitung für Online-Partien |
| `labels.csv` | Benannte Adressen im Spielcode |

## Urheberrecht

Copyright (c) 2026 [Erzoy38](https://github.com/Erzoy38). **Alle Rechte vorbehalten.**

Den Quellcode anzusehen ist erlaubt. Ohne schriftliche Erlaubnis darf man ihn nicht kopieren, weitergeben, verändern, darauf aufbauen oder als eigene Arbeit ausgeben. Einzelheiten stehen in [`LICENSE`](LICENSE). Anfragen gehen über GitHub an [Erzoy38](https://github.com/Erzoy38).

SDL3 steht unter der zlib-Lizenz.
