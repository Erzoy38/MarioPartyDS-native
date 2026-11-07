# Online spielen

Anleitung für eine Party mit bis zu vier PCs über das Internet. Ein Spieler, der **Host**, betreibt den Server auf seinem PC. Die anderen verbinden sich dorthin.

Jeder braucht `MarioPartyDS.exe` mit `SDL3.dll` und seine eigene ROM des Spiels. Woher die anderen das Programm bekommen, ist noch nicht entschieden (Abschnitt 11 in `MULTIPLAYER_ROADMAP.md`).

## Was der Host einmal einrichtet

1. **Server starten:** Im Startbildschirm „PLAY ONLINE“ → „Server hier starten“. Der Server öffnet ein eigenes Fenster mit seinem Protokoll und seinem Fingerabdruck, z. B. `fingerprint ABBA 6009 E224 1DF5 …`. Solange dieses Fenster offen ist, läuft der Server. Er läuft auch weiter, wenn das Spiel zum Startbildschirm zurückkehrt.
2. **Windows-Firewall:** Beim ersten Start fragt Windows, ob `MarioPartyDS-server.exe` Netzwerkzugriff bekommen soll.
   - Für Spieler im selben Heimnetz reicht „Private Netzwerke“.
   - Die Entscheidung liegt beim Host; ohne Freigabe erreichen nur Programme auf demselben PC den Server.
3. **Router:** Für Spieler außerhalb des Heimnetzes muss der Router den Port an diesen PC weiterleiten.
   - Der Port ist **TCP 47000**, wenn in den Einstellungen nichts anderes steht.
   - Das stellt man in der Oberfläche des Routers ein (oft „Portfreigabe“ oder „Portweiterleitung“). Das Spiel ändert am Router nichts.
4. **Adresse für die anderen:**
   - Im Heimnetz zeigt das Spiel die Adresse nach „Server hier starten“ an.
   - Von außen gilt die öffentliche Adresse des Anschlusses; sie steht in der Oberfläche des Routers.
   - Ändert sie sich oft, hilft ein DynDNS-Name, den viele Router anbieten.

## Jede Partie

1. Der Host wählt „Raum erstellen“ und gibt den anderen den **Code** (6 Zeichen) und die ersten vier Gruppen des **Fingerabdrucks** aus dem Serverfenster.
2. Die anderen tragen unter „SETTINGS“ → „ONLINE“ → „SERVER“ die Adresse des Hosts ein, z. B. `meinname.dyndns.org:47000`.
3. Danach wählen sie „PLAY ONLINE“ → „Raum beitreten“, geben den Code ein und wählen „Bereit“.
4. Im Raum steht unten „ID …“ mit vier Gruppen. Sie müssen mit dem Anfang des Fingerabdrucks übereinstimmen, den der Host genannt hat. Beim ersten Mal steht „(neu)“ dahinter.
5. Der Host startet, sobald alle bereit sind.

## Was geschützt ist

- Jede Verbindung zum Server ist verschlüsselt (AES-256-GCM, Schlüsselaustausch ECDH P-256, Windows-eigene Kryptografie). Wer mitliest, sieht weder Namen noch Codes noch Eingaben. Veränderte Daten werden erkannt; die Verbindung baut sich dann neu auf.
- Der Server weist sich mit seinem Schlüssel aus. Jedes Spiel merkt sich den Schlüssel eines Servers beim ersten Mal.
  - Meldet derselbe Server später einen anderen Schlüssel, verbindet sich das Spiel nicht.
  - Dann zuerst beim Host nachfragen. Hat er den Server wirklich neu eingerichtet (die Datei `MarioPartyDS-server.key` neben dem Server ist dann neu), lässt sich der alte Schlüssel unter „SETTINGS“ → „ONLINE“ → „FORGET SERVER KEY“ löschen.
- Der Server selbst sieht die Spieldaten unverschlüsselt. Das ist der PC des Hosts.

## Wenn etwas abbricht

- Kurze Abbrüche überbrückt das Spiel selbst; alle warten so lange. Das Fenster zeigt den Grund und die verbleibende Zeit.
- Wer länger als 2 Minuten weg ist, beendet die Partie für alle.
- Stürzt das Programm eines Gastes ab, bietet der Startbildschirm nach dem Neustart „BACK INTO THE ONLINE GAME“. Das Programm holt die Partie dann nach; die anderen warten so lange.
- Der Host kann nicht zurückkehren. Endet sein Programm, endet die Partie.
- Wird der Server neu gestartet, setzen die Spiele die Partie fort.
- Hohe Verzögerung (über etwa 250 ms zwischen Gast und Host) macht das Spiel langsamer.

## Für Tests

- `MarioPartyDS-server.exe --bind 127.0.0.1` nimmt nur Verbindungen von diesem PC an; dann fragt Windows nicht nach der Firewall.
- `--allow-plain` lässt zusätzlich unverschlüsselte Verbindungen zu. Das brauchen nur die Protokolltests (`tools/lobby_test.py`).
