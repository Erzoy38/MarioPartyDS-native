# MarioPartyDS-online

Lobby-Server und Relay für Online-Partien mit bis zu vier Spielern, dazu die Client-Seite der Lobby, die Verschlüsselung und die Testwerkzeuge.

Dieses Repo enthält **kein Spiel, keinen Spielcode und keine Spielinhalte**. Das Spielprogramm, das diesen Server benutzt, ist nicht Teil davon.

## Download

Fertige Programme für Windows stehen unter [Releases](../../releases): `MarioPartyDS-server.exe` und `lobby-client.exe`, ohne Installation, Windows 10 oder 11 (64 Bit).

## Wer sich verbindet

Ein Server nimmt Spieler von allen Plattformen des Spiels in denselben Raum auf:

| Plattform | Rolle | Verschlüsselung |
|---|---|---|
| Windows | Spielt mit und kann den Server selbst starten | Windows-Kryptografie (CNG) |
| Android 8.0 und neuer | Spielt mit, tritt einem Server bei | Mbed TLS, gleiche Verfahren |
| iOS und iPadOS (in Vorbereitung) | Spielt mit, tritt einem Server bei | Mbed TLS, gleiche Verfahren |

Handys und Tablets starten keinen eigenen Server; dafür braucht es einen Windows-PC. Das Protokoll ist für alle gleich. Clients mit Mbed TLS bestehen dieselben Tests gegen diesen Server wie Windows-Clients.

## Inhalt

| Pfad | Inhalt |
|---|---|
| `server/server.cpp` | `MarioPartyDS-server`: Räume mit 6-stelligem Code, Bereitschaft, Start, danach Relay der Spielnachrichten. Räume überleben kurze Verbindungsabbrüche und einen Neustart des Servers |
| `src/lobby_protocol.h` | Das Protokoll: Nachrichtentypen, Fehlercodes, Rahmenformat |
| `src/lobby.h`, `src/lobby.cpp` | Die Client-Seite der Lobby, ohne blockierende Aufrufe |
| `src/secure.h`, `src/secure.cpp` | Verschlüsselung mit der Kryptografie von Windows (CNG); die Clients auf Android und iOS nutzen dieselben Verfahren über Mbed TLS |
| `client/lobby_client.cpp` | `lobby-client`: Lobby von der Kommandozeile aus, für Tests und von Hand |
| `tools/lobby_rules.py` | Prüft jede Regel des Servers, 28 Tests |
| `tools/netsim.py` | Proxy, der Verzögerung, Schwankung, Verlust, Abbrüche und veränderte Bytes erzeugt und mitschneiden kann |

## Bauen

Windows mit den Visual Studio Build Tools (C++), CMake und Ninja aus Visual Studio:

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

Ergebnis: `build\MarioPartyDS-server.exe` und `build\lobby-client.exe`. Keine weiteren Bibliotheken.

## Testen

```
python tools/lobby_rules.py
```

Nimmt `build\MarioPartyDS-server.exe` und `build\lobby-client.exe`; andere Pfade mit `--server` und `--client`.

Startet für jeden Test einen eigenen Server auf `127.0.0.1` ab Port 47800 und schreibt Protokolle nach `run\lobbytest\`.

## Server betreiben

```
MarioPartyDS-server.exe [--port 47000] [--bind ADRESSE] [--key DATEI] [--input-delay N] [--idle-timeout S] [--allow-plain]
```

- Standardport ist TCP 47000. Spieler außerhalb des eigenen Netzes, auch Handys im Mobilfunknetz, brauchen eine Portweiterleitung am Router.
- Beim ersten Start legt der Server seinen Schlüssel `MarioPartyDS-server.key` neben sich an und gibt dessen Fingerabdruck aus. Die Datei ist geheim und gehört nicht in ein Repo.
- `--allow-plain` lässt auch unverschlüsselte Verbindungen zu. Das brauchen nur die Protokolltests.

## Sicherheit

- **Schlüsselaustausch:** ECDH P-256 je Verbindung. Der Server unterschreibt ihn mit seinem festen ECDSA-P-256-Schlüssel.
- **Verschlüsselung:** AES-256-GCM mit einem Schlüssel je Richtung, abgeleitet über HMAC-SHA256. Die Nonce ist ein Zähler je Richtung.
- **Vertrauen beim ersten Kontakt:** Der Client merkt sich den Schlüssel je Adresse in `known_servers.txt`. Meldet dieselbe Adresse später einen anderen Schlüssel, wird sie abgewiesen.
- **Grenze:** Ende-zu-Ende ist die Verschlüsselung nicht. Der Server sieht die Spielnachrichten im Klartext.

Die Kryptografie ist selbst zusammengesetzt und nicht von Dritten geprüft.

## Protokoll in Kürze

- **Rahmen:** Jede Nachricht hat die Form `u32 Länge, u8 Typ, Inhalt`, Zahlen in Little Endian.
- **Lobby:** `Hello`, `Create`/`Join`, `Pick`, `Ready`, `Start`.
- **Nach dem Start:** Dieselbe Verbindung wird zum Relay. Gäste senden an den Host, der Host antwortet mit `RelayTo` an einen bestimmten Gast.
- **Rückkehr:** Wer die Verbindung verliert, kommt mit `Resume` und dem Token aus `Started` zurück.

Einzelheiten stehen in `src/lobby_protocol.h`.

## Urheberrecht

Copyright (c) 2026 Erzoy38 (https://github.com/Erzoy38). Alle Rechte vorbehalten.

Ansehen ist erlaubt. Kopieren, Weitergeben, Verändern, darauf Aufbauen oder als eigene Arbeit Ausgeben ist ohne schriftliche Erlaubnis nicht gestattet. Einzelheiten stehen in `LICENSE`.
