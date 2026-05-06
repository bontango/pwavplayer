# PWAVplayer

Ein polyphoner WAV-Player für ESP32-Mikrocontroller, entwickelt für den Einsatz in Flipperautomaten. Das Gerät liest WAV-Dateien von einer SD-Karte und gibt sie über einen DAC aus – mehrere Tracks können gleichzeitig abgespielt und gemischt werden.

(C) 2025 colrhon.org – [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)

---

## Funktionen

- **Polyphonie**: Mehrere WAV-Dateien werden gleichzeitig abgespielt und in Echtzeit gemischt.
- **SD-Karte**: WAV-Dateien und Konfiguration werden von einer FAT-formatierten SD-Karte gelesen.
- **GPIO-Events**: Bis zu 10 Eingangspins lösen Soundwiedergaben aus (direkt oder binär-codiert für Williams System 11 / Gottlieb System 1/80/80A/80B).
- **Serielle Steuerung**: Sounds können über UART ausgelöst werden (RX auf GPIO 36, nur Empfang).
- **USB Config Editor**: Browser-basierter Editor (`webapp/`) zur Verwaltung der SD-Karte und Konfiguration über USB (Chrome/Edge).
- **WLAN/HTTP-Zugang (optional)**: Derselbe Editor kann das Gerät zusätzlich über WLAN ansprechen (HTTP-REST auf Port 8080), parallel zu USB. Nur STA-Modus, Zugangsdaten liegen in `config.txt`.
- **Dateiattribute**: Einzelne WAV-Dateien können als Loop, Hintergrundsound, Init-Sound oder mit Kill-Verhalten konfiguriert werden.
- **Soundgruppen**: Mehrere Sounds können zu einer Gruppe zusammengefasst werden, aus der zufällig oder sequenziell ausgewählt wird.
- **Firmware-Update**: Neue Firmware kann als `update.bin` auf der SD-Karte abgelegt und beim Bootvorgang automatisch eingespielt werden.

---

## Konfiguration

### config.txt

Die Datei `config.txt` muss im Wurzelverzeichnis der SD-Karte liegen. Jede Zeile hat das Format `schlüssel=wert`. Fehlende Einträge werden mit Standardwerten belegt.

| Schlüssel | Werte | Standard | Beschreibung |
|-----------|-------|----------|--------------|
| `dac` | `12`, `16` | `12` | Bit-Tiefe des DAC-Ausgangs |
| `mix` | `sum`, `div2`, `sqrt` | `div2` | Mischverfahren bei mehreren gleichzeitigen Tracks |
| `evt` | `none`, `flat`, `flat0`, `bw11`, `bg80`, `by35` | `bg80` | GPIO-Eventmodus |
| `deb` | Zahl (ms) | `10` | Entprellzeit in Millisekunden |
| `rpd` | Zahl (ms) | `60` | Ruheperiode in Millisekunden (nur `flat`) |
| `ser` | `none`, `uart` | `none` | Serielle Schnittstelle für Sound-Kommandos |
| `usbbaud` | `115200`, `230400`, `460800`, `921600` | `115200` | Baudrate des USB-Config-Editor-Ports (UART0) |
| `stheme` | String | `orgsnd` | Aktives Sound-Theme-Verzeichnis unterhalb der SD-Wurzel (Sounds und Gruppen werden von dort geladen) |
| `log` | `no`, `yes`, `only` | `no` | Persistentes Eventlog auf SD (`log.txt`); `only` schreibt das Log, unterdrückt aber die Audiowiedergabe |
| `volv` | 0–100 | `100` | Skalierung der Voice-Lautstärke in % (gilt für Dateien mit Attribut `v`) |
| `vols` | 0–100 | `100` | Skalierung der Sound-Lautstärke in % (gilt für Nicht-Voice-Dateien) |
| `wifi_enable` | `yes`, `no` | `no` | Aktiviert STA-WLAN und HTTP-Server auf Port 8080 |
| `wifi_ssid` | String | *(leer)* | WLAN-SSID — erforderlich bei `wifi_enable=yes` |
| `wifi_pwd` | String | *(leer)* | WLAN-Passwort |

**Beispiel:**
```
dac=12
mix=div2
evt=bg80
deb=10
rpd=40
ser=uart
usbbaud=921600
wifi_enable=yes
wifi_ssid=MeinNetz
wifi_pwd=geheim
```

> **Hinweis:** Fehlt die Datei `config.txt`, startet die Firmware mit den obigen Standardwerten.
> Der `usbbaud`-Default von `115200` lässt `idf.py monitor` ohne Zusatzparameter laufen.
> Für schnellere Dateiübertragungen im Editor kann z. B. `921600` gesetzt werden — dann
> muss auch das Baud-Drop-down im Editor vor dem nächsten Connect entsprechend angepasst werden.

---

### Mischverfahren (`mix`)

| Wert | Verhalten |
|------|-----------|
| `sum` | Alle Track-Samples werden addiert (kann übersteuern) |
| `div2` | Jeder Sample wird halbiert, dann addiert |
| `sqrt` | Samples werden addiert und durch Wurzel der Track-Anzahl geteilt |

---

### GPIO-Eventmodus (`evt`)

| Wert | Beschreibung |
|------|--------------|
| `none` | Keine GPIO-Events |
| `flat` | 10 Eingangspins, jeder Pin löst Sound mit derselben Nummer aus (ab Rel. 0.9.2, mit Ruheperiode) |
| `flat0` | Wie `flat`, ältere Variante ohne Ruheperiode (bis Rel. 0.9.1) |
| `bw11` | Williams System 11 – binär codierter Sound-Bus |
| `bg80` | Gottlieb System 1/80/80A/80B – binär codierter Sound-Bus |
| `by35` | Bally -35 / Stern MPU – binär codierter Sound-Bus, Einlesen per externem Strobe (GPIO34, positive Flanke) |

Bei `flat` / `flat0`: Pin 1–10 lösen Sound-ID 1–10 aus. Eingänge sind Active-Low mit externem 10-kΩ-Pull-up.

**Entprellung und Ruheperiode (`flat`-Modus):**
- `deb`: Nach einer erkannten Flanke wird der Pin für diese Dauer gesperrt.
- `rpd`: Innerhalb dieser Zeit nach dem letzten Event wird ein weiteres Event auf demselben Pin ignoriert und die Zeitfenster vorgeschoben.

---

### Serielle Schnittstelle (`ser`)

#### UART (`ser=uart`)
- RX auf **GPIO 36** (nur Eingang; kein interner Pull-up — bei Bedarf externen 10 kΩ Pull-up nach 3,3 V verwenden)
- TX wird nicht genutzt
- 115200 Baud, 8N1, kein Handshake
- Befehl zum Abspielen: `p <id>` gefolgt von Zeilenende — Beispiel: `p 19` spielt Sound-Datei 19 ab
- GPIO 21 und 22 bleiben unabhängig von dieser Einstellung dauerhaft für die on-board LEDs verfügbar

---

## Benennung von Sound-Dateien

Sound-Dateien liegen im aktiven Sound-Theme-Verzeichnis der SD-Karte
(`<sdroot>/<stheme>/`, Standard `<sdroot>/orgsnd/`) und folgen diesem
Namensschema:

```
NNNN-AAAA-VVV-beschreibung.wav
```

| Feld | Beschreibung |
|------|--------------|
| `NNNN` | 4-stellige numerische ID (z. B. `0012`) |
| `AAAA` | 4 Attributzeichen (s. Tabelle unten); ungenutzte Stellen mit `x` auffüllen |
| `VVV` | Lautstärke 0–100 |
| `beschreibung` | Beliebiger Textname (kann Bindestriche enthalten) |

**Beispiel:** `0012-bxxx-100-ring-my-bell.wav`

### Attribute

Die 4 Attributzeichen sind **positionsunabhängig** — die Reihenfolge spielt keine
Rolle, jedes Zeichen darf höchstens einmal vorkommen. Mit `x` auf vier Stellen
auffüllen.

| Zeichen | Bedeutung |
|---------|-----------|
| `l` | Loop — Datei wird endlos wiederholt |
| `b` | Break — laufende Instanz desselben Sounds wird gestoppt, bevor neu gestartet wird |
| `i` | Init/Hintergrund — wird beim Start automatisch abgespielt; wenn auch `l` gesetzt, als Hintergrundmusik-Loop |
| `v` | Voice — verwendet `volv` (Voice-Lautstärke) statt `vols` |
| `k` | Kill — alle laufenden Tracks werden gestoppt, bevor dieser Sound startet |
| `c` | Soft-Kill — alle Tracks außer Init/Hintergrund werden gestoppt |
| `q` | Quit — Soft-Kill, der Loops und Voice-Sounds erhält |
| `x` | Platzhalter (kein Attribut) |

`k`, `c` und `q` schließen sich gegenseitig aus — pro Datei nur ein Kill-Modus.

---

## Soundgruppen

Gruppen fassen mehrere Sounds zusammen. Beim Aufruf der Gruppen-ID wird ein Mitglied ausgewählt und abgespielt.

Gruppen-Dateien liegen ebenfalls im aktiven Theme-Verzeichnis
(`<sdroot>/<stheme>/`):

```
NNNN-A-M1-M2-M3-beschreibung.grp
```

| Feld | Beschreibung |
|------|--------------|
| `NNNN` | 4-stellige Gruppen-ID |
| `A` | Auswahlmodus: `m` = zufällig, `r` = sequenziell (Round-Robin) |
| `M1`, `M2`, … | Sound-IDs der Mitglieder |

**Beispiel:** `0009-m-15-71-12-exit-lane-left.grp` – zufällige Auswahl aus Sounds 15, 71 und 12.

---

## Gesprochene Versionsnummer

Beim Start wird automatisch eine gesprochene Versionsansage abgespielt, wenn die passende Datei vorhanden ist:

```
/spokenvers/version-X-Y-Z.wav
```

wobei X, Y, Z die drei Stellen der Versionsnummer sind (z. B. `version-0-9-3.wav` für Version 0.9.3).

---

## Firmware-Update

Eine neue Firmware kann auf die SD-Karte kopiert werden:

```
update.bin
```

Beim nächsten Bootvorgang prüft das Gerät ob die Datei vorhanden und gültig ist, spielt sie in die inaktive OTA-Partition ein und startet mit der neuen Version neu. Die Datei wird nach erfolgreichem Update nicht automatisch gelöscht.

---

## Config Editor (USB oder WLAN)

Im Verzeichnis `webapp/` befindet sich `PWAVplayer_config_editor.html` – ein browserbasierter Editor für Chrome oder Edge (Desktop). Er kann das Gerät wahlweise über **USB** (Web Serial) oder **WLAN** (HTTP-REST auf Port 8080) ansprechen. Der Modus wird im Device-Tab umgeschaltet; firmware­seitig laufen beide Transporte parallel.

### Funktionen

- **Konfiguration**: `config.txt` bearbeiten, auf das Gerät hochladen oder herunterladen (inkl. WLAN-Einstellungen im *General*-Tab)
- **Sound-Dateien**: Tabelle pro Slot (IDs 0001–0031) mit zwei Zuweisungs-Buttons — **PC** lädt eine lokale WAV hoch, **SD** wählt eine bereits auf der Karte vorhandene WAV aus und benennt sie auf den Slot um
- **Soundgruppen**: `.grp`-Dateien auf der SD-Karte anlegen, bearbeiten und löschen; Mitglieder-WAVs über Checkbox-Liste auswählen, Abspielmodus `m` (zufällig) oder `r` (Round-Robin)
- **SD-Karte**: Dateiliste anzeigen (Name, Größe, Datum), Dateien herunterladen, umbenennen und löschen
- **Firmware-Update**: Lokale `.bin`-Datei als `update.bin` auf die SD-Karte übertragen; vollständiges Flash-Utility über esptool-js (Browser-USB)
- **Standardkonfiguration**: Vorgabe-`config.txt` von lisy.dev laden
- **WLAN-Status (USB)**: Im *Device*-Tab fragt der Button **Get IP Status** den Verbindungszustand ab und überträgt die vergebene IP automatisch in das Adressfeld des IP-Modus
- **Debug-Tab**: Das USB-Protokoll-Log wurde in einen eigenen Tab ausgelagert

### Voraussetzungen

- Chrome oder Edge (Desktop) – Web Serial API erforderlich für USB-Modus
- ESP32 per USB verbunden, oder im LAN erreichbar (STA-WLAN)
- Firmware läuft (`UsbSerial`-Task ist immer aktiv; HTTP-Server startet nur bei `wifi_enable=yes` und erfolgreicher IP-Zuteilung)

### Protokolle

- **USB**: framebasiertes Textprotokoll auf `UART_NUM_0`; Baudrate per `usbbaud` (Standard `115200`).
- **HTTP**: REST auf Port 8080, CORS aktiv, keine Authentifizierung (für vertrauenswürdige LAN-Umgebung).

Details zu beiden Transporten siehe `webapp/API.md`, Bedienungshinweise zum Editor in
`webapp/README.md`.
