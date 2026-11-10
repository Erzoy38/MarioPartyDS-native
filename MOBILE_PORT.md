# Android und iOS: Portierungsplan

Stand 2026-11-10. Ziel: ein gemeinsamer Codekern für Windows, Android und iOS. Windows bleibt dabei,
wie es ist. Jeder Schritt wird geprüft: Das Spielbild bleibt bitgleich, und `check_native_boot` meldet „all equal“.

## Bestandsaufnahme

Der Port ist schon weitgehend plattformneutral:

| Teil | Dateien | Plattformbindung |
|---|---|---|
| Übersetzter Spielcode | `native/generated/` (237 Dateien, 151 MB) | keine: kein `#pragma`, keine MSVC-Intrinsics, kein `_WIN32` |
| CPU, Speicher, Hardware, BIOS, Overlays | `runtime/cpu,memory,hw,bios,overlay,machine` | keine |
| 2D (PPU) und 3D-Rasterer | `runtime/ppu.cpp`, `runtime/gpu3d.cpp` | keine (rechnet auf dem Prozessor) |
| Ton | `runtime/sound.cpp`, Ausgabe über `SDL_OpenAudioDeviceStream` | keine (SDL3) |
| Mikrofon | `src/microphone.cpp` | keine (SDL3); auf dem Handy braucht es eine Berechtigung |
| Fenster, Bild, Eingabe, Controller | `src/main.cpp`, `display.cpp`, `start_screen.cpp` | SDL3, aber fest auf `direct3d11` |
| Hochskalierung (glatt, AMD FSR 1) | `src/upscale.cpp` | keine (Prozessor) |
| Kein Rekompilierer zur Laufzeit | | Der Code ist vorab übersetzt (AOT), deshalb geht er auch auf iOS, wo JIT verboten ist |

**Windows-spezifische Abhängigkeiten (vollständig):**

1. **Netzwerk:** Winsock (`WSAStartup`, `closesocket`, `ioctlsocket`, `WSAGetLastError`) in `runtime/net.cpp`,
   `runtime/lobby.cpp`, `src/launcher.cpp` und `server/server.cpp`. Dazu `QueryPerformanceCounter` und `Sleep`
   in `net.cpp`.
2. **Verschlüsselung:** Windows CNG (`BCrypt*`) in `runtime/secure.cpp`. Gebraucht werden ECDH P-256,
   ECDSA P-256, SHA-256, HMAC-SHA-256, AES-GCM und ein Zufallsgenerator.
3. **Schrift:** GDI (`CreateFontW`, `TextOutW`) mit Segoe UI und Arial Black in `src/ui.cpp`.
4. **Server starten:** `CreateProcessW` startet `MarioPartyDS-server.exe` in `src/launcher.cpp`.
5. **Grafik:**
   - `SDL_HINT_RENDER_DRIVER = "direct3d11"` in `main.cpp`.
   - DLSS (`src/dlss.cpp`, D3D11 und NGX) gibt es nur für Windows mit NVIDIA.
6. **Dateien:**
   - `SDL_GetBasePath()` dient für textures/, mods/ und das Hintergrundbild „neben dem Programm“.
   - Die ROM wird über einen Dateipfad gelesen. Unter Android liefert die Dateiauswahl aber eine
     `content://`-Adresse.
7. **Build:**
   - MSVC-Schalter (`/W4 /WX /utf-8 /bigobj /Od`).
   - `ws2_32`, `bcrypt`, `d3d11`.
   - SDL3 liegt nur als fertige Windows-DLL vor (`third_party/SDL3`, Version 3.4.20). Die Quelltexte fehlen.

## Entscheidungen

- **Grafik:**
  - Das Bild läuft heute schon über SDL3s `SDL_Renderer`. Dieselbe Schicht hat Treiber für Vulkan,
    OpenGL ES und Metal, deshalb bleibt die Architektur.
  - Android: `vulkan`, Rückfall auf `opengles2`.
  - iOS: `metal`.
  - Die 3D-Auflösung (1x–11x), die Anordnung der Bildschirme und FSR 1 bleiben, weil sie auf dem Prozessor laufen.
  - DLSS gibt es nur auf Windows. Auf dem Handy fehlt die Einstellung.
- **Verschlüsselung auf Android und iOS:**
  - Dafür kommt Mbed TLS (Apache 2.0) zum Einsatz. Es kann alle fünf nötigen Verfahren.
  - Windows bleibt bei BCrypt. Die Schnittstelle `secure.h` bleibt gleich.
  - Das Protokoll bleibt ebenfalls gleich, damit Handy und PC zusammen spielen können.
- **Schrift:**
  - Auf dem Handy zeichnet `stb_truetype` (gemeinfrei, eine Kopfdatei) die Schrift.
  - Die Schriftart wird mitgeliefert, zum Beispiel Noto Sans (OFL), weil Segoe UI nicht weitergegeben werden darf.
  - Windows bleibt bei GDI.
- **Netzwerk:**
  - Eine kleine Schicht `runtime/platform_socket.h` deckt Winsock und POSIX-Sockets ab.
  - Unter Windows ruft sie genau dieselben Funktionen wie bisher auf.
- **ROM auf dem Handy:**
  - Die Datei wird einmal über die Dateiauswahl gewählt (SDL3: `SDL_ShowOpenFileDialog`).
  - Dann wird sie in den privaten App-Speicher kopiert. Sie verlässt das Gerät nicht.
- **Host-Server:** Den Server-Start gibt es nur auf Windows. Handys treten einer Partie bei.
- **Ordner:**
  - Texturpakete, Mods und Spielstand liegen auf dem Handy unter `SDL_GetPrefPath`.
  - Bei Android ist das der App-Speicher, bei iOS der Documents-Ordner. Über die Dateien-App lassen sich dort Texturen ablegen.

## Phasen

### Phase 0: Werkzeuge
- **Android:**
  - Android SDK (Kommandozeilen-Tools), NDK, JDK 17 und Gradle (über den Wrapper).
  - Alles kommt auf Laufwerk F: (`F:\Tools\Android`), weil C: nur 2 GB frei hat.
  - `GRADLE_USER_HOME` zeigt ebenfalls auf F:.
- **Quellen:** SDL3 3.4.20 als Quelltext (gleiche Version wie unter Windows), Mbed TLS, stb_truetype und Noto Sans.
- **iOS:** Dafür braucht es einen Mac mit Xcode. Hier kann ich die Projektdateien schreiben, aber weder
  übersetzen noch testen.

### Phase 1: Plattformschicht (nur Windows, nichts sichtbar anders), erledigt
- **Neue Dateien:**
  - `runtime/platform_socket.h`: Sockets, Zeit und Pausen für Winsock und POSIX.
  - `src/platform.h/.cpp`: Ordner, mitgelieferte Dateien und Grafiktreiber.
  - `src/text_raster.h` mit `text_raster_win.cpp`: der GDI-Teil aus `ui.cpp`.
  - `src/dlss_none.cpp`: DLSS-Ersatz für Handys.
- **Umgestellt:** `net.cpp`, `lobby.cpp`, `launcher.cpp`, `ui.cpp`, `display.cpp`, `start_screen.cpp` und `main.cpp`.
- **Bleibt nur Windows:** `server.cpp`, `secure.cpp` (CNG) und `dlss.cpp`.
- **CMake:** trennt MSVC- und Clang-Schalter. Für Android und iOS fehlen noch `secure_mbedtls.cpp` und
  `text_raster_font.cpp` (Phase 2) und das App-Ziel (Phase 3 und 6).
- **Unterschied auf POSIX:** Ein fehlgeschlagener Verbindungsaufbau meldet sich dort als beschreibbar mit
  `SO_ERROR`, nicht in der Fehlermenge. Das fängt `plat::connect_state` ab.

- Sockets, Zeitmessung, Schrift, Server-Start, Pfade und die Wahl des Grafiktreibers kommen hinter
  `platform_*`-Dateien, mit Windows- und POSIX-Variante.
- Prüfung:
  - Windows-Build und `--smoke-test`.
  - Spielbild nach 2400 Bildern bitgleich.
  - Boot-Vergleich „all equal“.
  - Online-Test PC gegen PC.

### Phase 2: Übersetzen mit Clang, erledigt (bis auf den Lauf auf Android)
- **Werkzeuge:**
  - Android NDK r29 liegt in `F:\Tools\Android\android-ndk-r29`.
  - In `native/third_party/` liegen SDL3 3.4.20 als Quelltext (`SDL3-src`), Mbed TLS 3.6.4,
    `stb/stb_truetype.h` und `fonts/` (Noto Sans Regular, SemiBold und Black, mit OFL.txt).
- **Verschlüsselung:**
  - `secure.cpp` enthält jetzt nur noch Handshake, Datensätze und bekannte Server.
  - Die Bausteine kommen aus `secure_crypto.h`, umgesetzt in `secure_cng.cpp` (Windows) und
    `secure_mbedtls.cpp`. Der rohe ECDH-Wert wird dort wie bei CNG umgedreht (niedrigstes Byte zuerst).
  - Das Schlüsselformat entspricht CNG-Blobs.
  - Geprüft mit `-DMPDS_CRYPTO=mbedtls` (`native/build-mbedtls`): Alle Lobby-Regeln und das Spiel zu viert
    bestehen mit Mbed-TLS-Spielern gegen den Windows-Server.
- **Schrift:**
  - `text_raster_font.cpp` zeichnet mit stb_truetype nach GDI-Maßen: Em-Größe, Zeile =
    Ascent + Descent, Abstand nach jedem Buchstaben.
  - Mit `-DMPDS_TEXT=font` auch unter Windows ansehbar; der Startbildschirm behält sein Layout.
- **Clang:**
  - `native/build_android_runtime.ps1 [-Abi arm64-v8a|x86_64]` baut Runtime, Spielcode und `MarioPartyDS-headless`.
  - Behoben wurden nur `fopen_s` und `_stricmp`, über `runtime/platform_c.h`.
  - Keine Clang-Warnungen in der Runtime.
  - Der Spielcode mit `-O1` braucht etwa 2 Minuten. Ohne Debug-Infos ist das Programm 46 MB groß (arm64).
- **Offen:** Der Lauf auf Android (Boot-Vergleich im Emulator) braucht den Emulator aus Phase 3.

- Runtime und Spielcode werden mit dem NDK-Clang für arm64 übersetzt. Die Fehler, die MSVC durchgehen
  ließ, werden behoben.
- Der Spielcode bekommt `-O1`, nicht wie unter Windows `/Od`. Auf dem Handy wäre er sonst zu langsam.
  Die Bauzeit wird gemessen.
- Prüfung: Die Boot-Prüfung (`MarioPartyDS-headless`) läuft als Android-Programm im Emulator mit Abbild
  und muss gleiche Speicherabzüge ergeben.

### Phase 3: Android-Projekt
- Gradle-Projekt `android/` auf Basis der `android-project`-Vorlage aus den SDL3-Quellen (`SDLActivity`).
- CMake bindet `native/` ein.
- Ziele: arm64-v8a für Geräte, x86_64 für den Emulator.
- Ergebnis: `assembleRelease` (APK) und `bundleRelease` (AAB), signiert mit einem eigenen Schlüssel, der lokal bleibt.
- Prüfung: Start, Startbildschirm, ROM wählen, Titelbild und ein Minispiel im Emulator, wenn möglich auf
  einem echten Gerät.

### Phase 4: Steuerung und Bildschirm
- **Touch-Steuerung:**
  - Virtuelles Steuerkreuz und die Tasten A, B, X, Y, L, R, Start und Select.
  - Tippen auf den unteren DS-Bildschirm wirkt als Stift.
- **Editor:** Tasten verschieben, Größe und Deckkraft einstellen. Gespeichert wird je Ausrichtung.
- **Bluetooth-Controller:** über SDL3-Gamepad wie unter Windows. Bei angeschlossenem Controller blendet
  sich die Touch-Steuerung aus.
- **Hochformat:** Die Bildschirme stehen übereinander oben, die Tasten liegen darunter.
- **Querformat:**
  - Nebeneinander oder Hybrid in der Mitte, die Tasten an den Seiten.
- **Bildschirm:** Sicherer Bereich über `SDL_GetWindowSafeArea` (Notch, Kamera-Loch). Jedes
  Seitenverhältnis wird mit Rand dargestellt, nichts wird abgeschnitten.
- **App-Wechsel:** Im Hintergrund hält das Spiel an, der Ton pausiert, und der Spielstand wird geschrieben.

### Phase 5: Ton
- Die Puffergröße kommt über `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES`, die Latenz wird gemessen.
- Ton-Aussetzer werden im Dauerlauf gezählt.
- Für das Mikrofon braucht Android `RECORD_AUDIO`, iOS `NSMicrophoneUsageDescription`.

### Phase 6: iOS
- **CMake:** mit `CMAKE_SYSTEM_NAME=iOS`. `cmake -G Xcode` erzeugt daraus auf dem Mac das Xcode-Projekt.
  Das ist verlässlicher als eine von Hand geschriebene `.pbxproj`.
- **App-Dateien:** `Info.plist`, App-Symbol, Startbildschirm und Metal-Renderer.
- **IPA:** Die Export-Einstellungen (`ExportOptions.plist`) gehören dazu. Das Signieren braucht ein
  eigenes Apple-Entwicklerkonto.
- **Prüfung:** nur auf dem Mac (Simulator oder iPhone/iPad), durch dich oder einen Mac, auf den ich zugreifen kann.

### Phase 7: QA je Plattform
- **Gleichheit:**
  - Spielbild und Boot-Vergleich wie bei Windows.
  - Online Handy gegen PC.
- **Geschwindigkeit:** Leistung auf 1x/2x und Speicherverbrauch im Dauerlauf.
- **Paket:**
  - Es enthält weder ROM noch Spielstand noch Schlüssel.
  - APK, AAB und IPA enthalten den übersetzten Spielcode. Sie bleiben deshalb wie die Exe privat.

## Risiken

- **Geschwindigkeit:** Spielcode und 3D-Rasterer laufen auf dem Prozessor. Auf einem Handy sind 1x und 2x
  realistisch, 11x nicht. Gemessen wird in Phase 3.
- **Bauzeit:** 151 MB generierter Code mit Clang und Optimierung, mal zwei Android-ABIs.
- **Online zwischen Plattformen:** Das Spiel muss auf ARM genau gleich laufen wie auf x86. Es rechnet mit
  ganzen Zahlen, deshalb ist das wahrscheinlich. Geprüft wird es in Phase 7.
- **Eigene Grafik-Treiber:** SDLs Vulkan-Renderer läuft nicht auf jedem älteren Android-Gerät, deshalb gibt
  es den Rückfall auf OpenGL ES.
