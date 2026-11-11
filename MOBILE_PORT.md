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

### Phase 3: Android-Projekt, gebaut (ohne Test auf Gerät oder Emulator)
- **Bauen:** `powershell -ExecutionPolicy Bypass -File android\build.ps1` (Werkzeuge in `F:\Tools\Android`).
  - Ergebnis: `app-release.apk` (123 MB) und `app-release.aab` (79 MB), signiert mit CN=Erzoy38,
    Zertifikat SHA-256 `929b3285…645ebb`. Paket `io.github.erzoy38.mariopartyds`, minSdk 26, targetSdk 35.
  - Werkzeuge: AGP 8.7.3, Gradle 8.12, JDK 17, NDK r29, CMake 3.31.6 (CMake 4 lehnt das alte
    Mindestversion-Feld von Mbed TLS ab).
  - Temporäre Dateien liegen in `F:\Tools\Android\tmp`; auf C: reichte der Platz für bundletool nicht.
- **Lokal, nie committen:** `android/local.properties`, `android/keystore.properties` und
  `F:\Tools\Android\keys\mpds-release.jks`. Ohne Schlüssel und Passwort gibt es keine Updates der App.
- **Aufbau:** SDL3 aus den Quellen als `libSDL3.so`, das Spiel als `libmain.so` (`native/CMakeLists.txt`,
  Zweig `ANDROID`). Die Java-Seite ist SDLs `SDLActivity`; `MainActivity` nennt nur die Bibliotheken.
  Schriften und Startbild kommen als Assets ins APK.
- **Code:**
  - `platform::keep_rom` kopiert die gewählte ROM (auf Android eine `content://`-Adresse) nach
    `rom.nds` im App-Ordner, damit sie beim nächsten Start ohne erneute Freigabe da ist.
  - Der DLSS-Hinweis steht nur unter Windows.
  - „Zurück zum Menü“ beendet auf dem Handy vorerst die App (Phase 4).
- **Windows geprüft:** Bild nach 2400 Bildern gleich wie v0.3.6 (`ADF748D445B5A938`), Boot-Prüfung gleich.
- **Offen:** Start auf Gerät oder Emulator. Dieser PC hat keinen Hypervisor für den Emulator.

- Gradle-Projekt `android/` auf Basis der `android-project`-Vorlage aus den SDL3-Quellen (`SDLActivity`).
- CMake bindet `native/` ein.
- Ziele: arm64-v8a für Geräte, x86_64 für den Emulator.
- Ergebnis: `assembleRelease` (APK) und `bundleRelease` (AAB), signiert mit einem eigenen Schlüssel, der lokal bleibt.
- Prüfung: Start, Startbildschirm, ROM wählen, Titelbild und ein Minispiel im Emulator, wenn möglich auf
  einem echten Gerät.

### Phase 4: Steuerung und Bildschirm, umgesetzt (unter Windows geprüft, auf Android nur gebaut)
- **Touch-Steuerung:** `src/touch_controls.h/.cpp` mit Steuerkreuz (8 Richtungen), A B X Y (zwischen
  zwei Tasten beide), L, R, Start, Select und einer Menütaste. Mehrere Finger gleichzeitig; ein Finger
  darf von Taste zu Taste gleiten. Ein Finger auf dem unteren DS-Bildschirm ist der Stift.
- **Spielmenü** (Menütaste oder Android-Zurück): Weiter, Bildschirme tauschen, Tasten anpassen,
  Tasten aus- und einblenden, Zurück zum Hauptmenü (zweimal tippen). Solange es offen ist, bekommt das
  Spiel keine Eingaben; es läuft weiter wie bei der Esc-Frage, weil online andere mitspielen.
- **Editor:** Tasten ziehen, antippen wählt; Kleiner/Größer (0,5–2), Blasser/Kräftiger (0,15–1),
  Standard, Fertig. Gespeichert je Ausrichtung in `touch_controls.ini` im App-Ordner.
- **Hoch- und Querformat:** Hochformat: Bildschirme übereinander oben (höchstens 62 % der Höhe), Tasten
  darunter. Querformat: Anordnung aus den Einstellungen, Tasten an den Seiten, bei „nebeneinander“
  halbdurchsichtig darüber. Alles innerhalb von `SDL_GetWindowSafeArea`, mit Rand statt Abschneiden.
- **Controller:** Ein Tastendruck am Gamepad blendet die Touch-Tasten aus, eine Berührung wieder ein.
- **Hintergrund:** Beim Wechsel in den Hintergrund wird ein offener Spielstand sofort geschrieben, der Ton
  pausiert. Zurück im Vordergrund wird der Tonpuffer geleert, und das Spiel holt keine Zeit nach.
- **Zurück zum Hauptmenü:** Das Spiel startet nur einmal je Prozess. Auf Android ruft
  `platform::restart_app` per JNI `MainActivity.restartApp`; `RestartActivity` (eigener Prozess
  `:restart`) beendet den Spielprozess und öffnet die App neu. Eine fertige Activity beendet ihren
  Prozess, damit jeder Start frisch beginnt. iOS kann das nicht (Phase 6).
- **Zurück-Taste** in Startbildschirm und Online-Menü wie Esc, nur auf Handys.
- **Windows:** Ohne `--touch-controls` unverändert: Spielbild (`ADF748D445B5A938`) und Fensterbild nach
  2400 Bildern gleich wie v0.3.6, Boot-Prüfung gleich. Mit `--touch-controls` wird die Maus zum Finger
  (`SDL_HINT_MOUSE_TOUCH_EVENTS`); so wurden Menü, Editor, Speichern, Stift und A-Taste mit echten
  Klicks geprüft (`tools/test_window_input.ps1`, neuer Schritt `pixel=X;Y`).
- **Offen:** Startbildschirm und Online-Menü beachten den sicheren Bereich noch nicht (sie sind mit
  Rand eingepasst). Test auf einem Gerät steht aus.

Ursprünglicher Plan:
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
Stand: umgesetzt (unter Windows geprüft, auf Android nur gebaut).
- **Gerätepuffer:** Auf Handys setzt `main.cpp` vor `SDL_Init` `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES=256`
  (bei 48 kHz etwa 5 ms statt SDLs 20 ms). AAudio läuft bei SDL ohnehin im Modus `LOW_LATENCY`
  (`SDL_HINT_ANDROID_LOW_LATENCY_AUDIO`, Vorgabe an). Windows behält SDLs Vorgabe.
- **Warteschlange (`SoundOut` in `main.cpp`):** Das Spiel liefert je Bild etwa 16,7 ms Ton. Unter Windows
  wie bisher: Ab 250 ms Wartendem wird ein Bild Ton verworfen. Auf Handys höchstens 100 ms; ist die
  Warteschlange ganz leer (Aussetzer), kommen zuerst zwei Bilder Stille dazu, damit ein etwas spätes Bild
  nicht gleich den nächsten Aussetzer macht.
- **Messung:** Alle 10 s eine Zeile `sound:` im Log mit mittlerer, kleinster und größter Wartezeit,
  Gerätepuffer, Aussetzern und verworfenen Bildern. Auf Handys immer, unter Windows mit `--audio-stats`.
  Windows, 2400 Bilder: etwa 10 ms in der Warteschlange plus 441 Frames (10 ms) Gerätepuffer, 0 Aussetzer,
  0 verworfen.
- **Mikrofon:** `RECORD_AUDIO` steht im Manifest. Die Abfrage zur Laufzeit macht SDL selbst beim ersten
  Öffnen eines Aufnahmegeräts (`SDL_aaudio.c`, `SDL_RequestAndroidPermission`); das Spiel wartet so lange.
  Abgelehnt: Das Spiel hört nichts, Einstellungen zeigen „no microphone“. Auf dem Handy erklärt der
  Einstellungstext das, ohne Windows- und Leertasten-Hinweise. iOS: `NSMicrophoneUsageDescription` kommt mit
  der `Info.plist` in Phase 6.
- **Windows:** Spielbild nach 2400 Bildern gleich (`ADF748D445B5A938`), mit und ohne `--audio-stats`;
  `--smoke-test` läuft.
- **Offen:** Latenz und Aussetzer auf einem echten Gerät messen (Log lesen). Falls 256 Frames auf einem
  Gerät knacken, ist der nächste Schritt 512.

### Phase 6: iOS
Stand: vorbereitet. Gebaut und getestet wird auf einem Mac; hier ist nur geprüft, was ohne Mac geht.
- **Bauen auf dem Mac:** `ios/build.sh` (Xcode, CMake ab 3.25).
  - `MPDS_IOS_TEAM=<Team-ID> ios/build.sh` erzeugt `native/build-ios/ipa/MarioPartyDS.ipa` für eigene,
    registrierte Geräte (Export `development`, automatische Signatur, `ios/ExportOptions.plist`).
  - `ios/build.sh --simulator` baut ohne Apple-Konto für den Simulator (Apple-Silicon-Mac) und nennt die
    `xcrun simctl`-Befehle zum Installieren und Starten.
  - Vorher nötig: `native/generated/` (von Windows kopieren oder `python3 tools/recomp.py`) und
    `native/third_party/` wie für Android.
- **CMake:** Zweig `CMAKE_SYSTEM_NAME STREQUAL "iOS"` in `native/CMakeLists.txt`, für `cmake -G Xcode`.
  SDL3 aus `third_party/SDL3-src` als statische Bibliothek in der App, Zeichnen über Metal
  (`preferred_render_driver`). Bundle-ID `io.github.erzoy38.mariopartyds`, iPhone und iPad, ab iOS 15, arm64.
  In die App kommen das Symbol (`ios/Assets.xcassets`), die Schriften unter `fonts/` und, falls lokal
  vorhanden, `menu_background.png`.
- **`ios/Info.plist.in`:** Version und Bundle-ID setzt CMake ein. Startbildschirm über `UILaunchScreen`
  (ohne Storyboard), Hoch- und Querformat, Vollbild ohne Split View, `NSMicrophoneUsageDescription`,
  `NSLocalNetworkUsageDescription` (Server im eigenen Netz), Copyright „Erzoy38“.
- **App-Symbol:** `tools/ios_icon.py` zeichnet dasselbe Bild wie das Android-Symbol als 1024×1024-PNG
  ohne Alphakanal; iOS rundet die Ecken selbst.
- **Ordner:** `user_folder()` ist auf iOS der Documents-Ordner der App. Er erscheint in der Dateien-App
  unter „Auf meinem iPhone > Mario Party DS“ (`UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`).
  Dort liegen auch Spielstand, Einstellungen und Log (`log_path`).
- **ROM:** SDL 3.4 hat auf iOS keine Dateiauswahl (`SDL_ShowOpenFileDialog` ist dort nur ein Platzhalter).
  Die `.nds`-Datei wird deshalb mit der Dateien-App in den App-Ordner kopiert; „ROM IMAGE“ oder „Spielen“
  nimmt die erste `.nds` dort (`rom_in_user_folder`), sonst steht ein Hinweis da. Sie wird nicht noch
  einmal kopiert (`keep_rom`).
- **Zurück zum Hauptmenü:** Eine iOS-App kann sich nicht selbst neu starten. Das Spiel sagt
  „The app closes now. Open it again to get to the main menu.“ und beendet sich.
- **Ende:** Auf iOS läuft eine App nach dem Ende von `main` ohne Bild weiter. `main` ruft deshalb
  `platform::app_ended`, das dort den Prozess beendet. Unter Windows und Android ändert sich nichts.
- **Hintergrund:** Auf Handys wartet die Spielschleife zwischen `WILL_ENTER_BACKGROUND` und
  `DID_ENTER_FOREGROUND` (`SDL_WaitEventTimeout`). Android hält SDLs Schleife ohnehin an, iOS nicht, und
  dort darf eine App im Hintergrund nicht zeichnen.
- **Netz:** Die Sockets setzen schon `SO_NOSIGPIPE`, ein abgebrochenes Spiel beendet die App also nicht.
- **Unter Windows geprüft:**
  - Alle App-Quellen übersetzen mit Clang auf dem iOS-Pfad (`__APPLE__`, `TARGET_OS_IPHONE`, SDL meldet
    `SDL_PLATFORM_IOS`) ohne Fehler und Warnungen, gegen die Header aus dem NDK.
  - `Info.plist` (mit eingesetzten Werten) und `ExportOptions.plist` lassen sich mit `plistlib` lesen,
    die `Contents.json` sind gültiges JSON, und `bash -n ios/build.sh` meldet nichts.
  - Windows unverändert: Spielbild nach 2400 Bildern `ADF748D445B5A938`, `--smoke-test` läuft.
- **Offen (braucht einen Mac):** CMake-Konfiguration mit Xcode, Übersetzen mit dem iOS-SDK, Start im
  Simulator, auf einem iPhone oder iPad: Metal, Touch, Ton, Mikrofonabfrage, Dateien-App und Hintergrund.
  Eine `.ipa` braucht ein Apple-Entwicklerkonto; mit einer kostenlosen Apple-ID geht nur „Run“ aus Xcode
  auf ein angeschlossenes Gerät (7 Tage gültig).

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
