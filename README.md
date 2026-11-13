# MarioPartyDS-native

**Mario Party DS als natives Programm für Windows und Android (iOS in Vorbereitung), mit Online-Multiplayer für bis zu vier Spieler.**

Der ARM-Code des Spiels wird automatisch nach C++ übersetzt (statische Rekompilierung). Eine eigene Laufzeit ersetzt die Hardware des Nintendo DS: CPU, Speicher, 2D- und 3D-Grafik, Sound, Touchscreen, Mikrofon und Spielstand. Die Originallogik bleibt dabei erhalten, Bretter und Minispiele müssen nicht neu geschrieben werden. Den Funkmodus des DS ersetzt eine verschlüsselte Internet-Verbindung über einen eigenen Server.

Alle Plattformen teilen sich denselben Kern (Laufzeit und übersetzter Spielcode). Nur Fenster, Grafiktreiber, Eingabe, Ton und Dateiablage sind je Plattform getrennt (`native/src/platform.*`). Ein PC und ein Handy spielen dieselbe Partie, weil das Spiel auf x86 und ARM gleich rechnet.

> ⚠️ **Der Quellcode in diesem Repository enthält kein Spiel.** Keine ROM, keine Grafiken, keine Musik und keinen übersetzten Spielcode. Wer ihn benutzen will, braucht eine eigene, rechtmäßig erworbene Kopie von *Mario Party DS* (Europa, Rev 1). Fertige Programme (Exe, APK, IPA) enthalten übersetzten Spielcode und sind nur für den privaten Gebrauch, nicht zur Weitergabe. „Mario Party DS“ und alles, was dazugehört, gehört den jeweiligen Rechteinhabern. Das Projekt steht in keiner Verbindung zu Nintendo oder Hudson Soft.

---

## Stand

Das Projekt wird aktiv entwickelt. Die Einzelheiten mit Messwerten stehen in [`MULTIPLAYER_ROADMAP.md`](MULTIPLAYER_ROADMAP.md) (Spiel und Online) und [`MOBILE_PORT.md`](MOBILE_PORT.md) (Android und iOS).

### Spiel

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

### Plattformen

| Plattform | Stand |
|---|---|
| Windows 10/11 | ✅ Direct3D 11, Tastatur, Maus, Controller; optional NVIDIA DLSS |
| Android 8.0 und neuer | 🚧 Läuft auf einem Xiaomi Redmi Note 11 5G bis ins Hauptmenü; Vulkan mit Rückfall auf OpenGL ES, Touch-Steuerung, Bluetooth-Controller |
| Online zwischen PC und Handy | 🚧 Unter Windows mit einem ARM-ähnlichen Build geprüft: gleicher Spielzustand, gleiches Bild; über ein echtes Netz noch nicht |
| iOS und iPadOS 15 und neuer | 🚧 Vorbereitet (Xcode-Projekt über CMake, Metal), noch nicht gebaut: braucht einen Mac |

✅ funktioniert · 🚧 funktioniert mit Einschränkungen oder ist noch nicht vollständig getestet

## Funktionen

- Läuft nativ, ohne Emulator: Windows und Android, iOS folgt
- Startbildschirm mit ROM-Auswahl, Einstellungen für Sprache, Fenstergröße und Vollbild
- Windows: Tastatur, Maus als Touchscreen und Controller
- Handy und Tablet: Touch-Tasten im Hoch- und Querformat, frei verschieb- und skalierbar, ausblendbar; der Finger auf dem unteren DS-Bildschirm ist der Stift; Bluetooth-Controller; Mikrofon zum Pusten
- Online-Partien für bis zu vier Spieler, mit 6-stelligem Raumcode, auch gemischt zwischen PC und Handy
- Eigener Server, den der Host direkt aus dem Spiel starten kann (Windows)
- Verschlüsselte Verbindung (ECDH P-256, AES-256-GCM) mit Server-Fingerabdruck: unter Windows mit den Bordmitteln (CNG), auf Android und iOS mit Mbed TLS
- Kurze Verbindungsabbrüche werden überbrückt, und ein Gast kommt nach einem Absturz wieder in die laufende Partie

## Voraussetzungen

Für alle Plattformen:

- Die eigene ROM: `Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).nds`
- Python 3

Windows-Programm:

- Windows 10 oder 11
- Visual Studio Build Tools mit C++ (CMake und Ninja sind dabei)
- [SDL3](https://github.com/libsdl-org/SDL/releases): das entpackte `SDL3-devel-*-VC.zip` unter `native/third_party/SDL3`

Android-App zusätzlich:

- JDK 17, Android SDK (Plattform 35, Build-Tools 35.0.0, CMake 3.31.6) und NDK r29
- Unter `native/third_party/`: SDL3 als Quelltext (`SDL3-src`), `mbedtls-3.6.4`, `stb/stb_truetype.h` und `fonts/` (Noto Sans mit OFL.txt)
- `android/local.properties` (`sdk.dir`, `mpds.ndkPath`) und für signierte Builds `android/keystore.properties`; beide bleiben lokal

iOS-App zusätzlich: ein Mac mit Xcode und CMake 3.25 oder neuer; für eine `.ipa` ein Apple-Entwicklerkonto.

## Bauen

Die übersetzten Spieldateien werden auf dem eigenen Rechner aus der eigenen ROM erzeugt und nie ins Repository hochgeladen.

```powershell
# 1. ROM einlesen und entpacken (schreibt nach unpacked/)
python tools/nds_header.py "<pfad zur rom>.nds"
python tools/decompress.py "<pfad zur rom>.nds"

# 2. Spielcode nach C++ übersetzen (schreibt nach native/generated/)
python tools/recomp.py
```

### Windows

```powershell
# Bauen, danach läuft automatisch ein kurzer Start-Test
powershell -ExecutionPolicy Bypass -File native\build.ps1
```

Das Ergebnis liegt in `native\build\`:

| Datei | Zweck |
|---|---|
| `MarioPartyDS.exe` | Das Spiel mit Fenster, Ton und Eingabe |
| `MarioPartyDS-server.exe` | Server für Online-Partien, enthält keinen Spielcode |
| `MarioPartyDS-headless.exe` | Ohne Fenster, für Tests und den Abgleich mit melonDS |

### Android

```powershell
# Signierte APK (zum direkten Installieren) und App Bundle (AAB), für arm64 und x86_64
powershell -ExecutionPolicy Bypass -File android\build.ps1 -Tools <ordner mit jdk-17*, sdk, android-ndk-r29>
```

Das Ergebnis liegt in `android\app\build\outputs\` (`apk\release\app-release.apk`, `bundle\release\app-release.aab`). Ohne `keystore.properties` sind die Release-Builds unsigniert.

Auf einem weiteren Windows-PC richtet `tools\setup_laptop.ps1` alles ein: Werkzeuge, die privaten Projektdaten, den Schlüssel, und baut danach die APK. Siehe [`MOBILE_PORT.md`](MOBILE_PORT.md), Abschnitt „Zweiter Rechner“.

### iOS (auf einem Mac)

```bash
ios/build.sh --simulator               # für den iOS-Simulator, ohne Apple-Konto
MPDS_IOS_TEAM=<Team-ID> ios/build.sh   # .ipa für eigene, registrierte Geräte
```

`native/generated/` und `native/third_party/` müssen dafür auf dem Mac liegen. Einzelheiten in [`MOBILE_PORT.md`](MOBILE_PORT.md), Phase 6.

## Spielen

### Windows

1. `MarioPartyDS.exe` starten.
2. Im Startbildschirm **SELECT ROM IMAGE** wählen und die ROM-Datei angeben. Ab dann heißt der Punkt **START GAME**.
3. Unter **SETTINGS** lassen sich Sprache, Fenstergröße, Vollbild, der eigene Name und die Server-Adresse einstellen.

Spielstand, Einstellungen und Protokoll liegen unter `%APPDATA%\MarioPartyDS\MarioPartyDS\`.

### Android

1. Die APK installieren (Installation aus dieser Quelle einmal erlauben). Eine neuere APK lässt sich über die alte installieren, Spielstand und Einstellungen bleiben.
2. **SELECT ROM IMAGE** antippen und die `.nds` in der Dateiauswahl wählen. Die App legt eine Kopie in ihrem eigenen Speicher ab.
3. Die Touch-Tasten liegen im Hochformat unter den DS-Bildschirmen, im Querformat links und rechts daneben.
4. Der Knopf **☰** (oder die Zurück-Taste) öffnet das Menü: Bildschirme tauschen, Tasten anpassen (verschieben, Größe, Deckkraft), Tasten ausblenden und zurück zum Hauptmenü. Bei ausgeblendeten Tasten bekommt das Spiel den ganzen Bildschirm, und der Menüknopf sitzt klein oben links.
5. Ein Tastendruck am Controller blendet die Touch-Tasten aus, eine Berührung blendet sie wieder ein.

### iOS

**SELECT ROM IMAGE** öffnet die Dateiauswahl des Systems (iCloud Drive, Downloads, „Auf meinem iPhone“); die App legt eine Kopie der `.nds` in ihrem eigenen Ordner ab. Eine iOS-App kann sich nicht selbst neu starten: „Zurück zum Hauptmenü“ schließt sie, danach öffnet man sie wieder.

### Steuerung am PC

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

Ein Spieler ist der **Host**: Er startet im Spiel unter **PLAY ONLINE → Server hier starten** den Server und erstellt einen Raum. Die anderen tragen seine Adresse ein und treten mit dem Raumcode bei, vom PC wie vom Handy. Der Server läuft standardmäßig auf **TCP-Port 47000**. Spieler außerhalb des Heimnetzes brauchen dafür eine Portweiterleitung am Router des Hosts. Den Server startet ein Windows-PC; Handys treten nur bei.

Die vollständige Anleitung mit Firewall, Router, Fingerabdruck und dem Verhalten bei Abbrüchen steht in [`ONLINE.md`](ONLINE.md).

## Aufbau des Repositorys

| Pfad | Inhalt |
|---|---|
| `native/runtime/` | Laufzeit: CPU, Speicher, Hardware, 2D (`ppu`), 3D (`gpu3d`), Sound, Netz, Lobby, Verschlüsselung |
| `native/src/` | Programm: Startbildschirm, Menüs, Eingabe, Touch-Steuerung (`touch_controls`), Plattformschicht (`platform`) |
| `native/server/` | Lobby- und Relay-Server |
| `android/` | Android-Projekt (Gradle): Activity, Manifest, App-Symbol, Build-Skript |
| `ios/` | iOS: `Info.plist`, App-Symbol, Export-Einstellungen, Build-Skript für den Mac |
| `tools/` | Python-Werkzeuge: Rekompilierer, Disassembler, Entpacker, Abgleich mit melonDS, Netz- und Minispiel-Tests; `setup_laptop.ps1` |
| `REVERSE.md` | Notizen zum Reverse Engineering: Speicherbelegung, Overlays, Dateiformate |
| `MULTIPLAYER_ROADMAP.md` | Architektur, Phasen und Messergebnisse des Online-Teils |
| `MOBILE_PORT.md` | Android- und iOS-Port: Entscheidungen, Phasen, Prüfungen |
| `ONLINE.md` | Anleitung für Online-Partien |
| `labels.csv` | Benannte Adressen im Spielcode |

## Urheberrecht

Copyright (c) 2026 [Erzoy38](https://github.com/Erzoy38). **Alle Rechte vorbehalten.**

Den Quellcode anzusehen ist erlaubt. Ohne schriftliche Erlaubnis darf man ihn nicht kopieren, weitergeben, verändern, darauf aufbauen oder als eigene Arbeit ausgeben. Einzelheiten stehen in [`LICENSE`](LICENSE). Anfragen gehen über GitHub an [Erzoy38](https://github.com/Erzoy38).

Verwendete Bibliotheken: SDL3 (zlib-Lizenz), Mbed TLS (Apache 2.0), stb_truetype (Public Domain / MIT), Noto Sans (SIL Open Font License).
