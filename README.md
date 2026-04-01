# PWAVplayer

Ein polyphoner WAV-Player für ESP32-Mikrocontroller, entwickelt für den Einsatz in Flipperautomaten. Das Gerät liest WAV-Dateien von einer SD-Karte und gibt sie über einen DAC aus – mehrere Tracks können gleichzeitig abgespielt und gemischt werden.

(C) 2025 colrhon.org – [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)

---

## Funktionen

- **Polyphonie**: Mehrere WAV-Dateien werden gleichzeitig abgespielt und in Echtzeit gemischt.
- **SD-Karte**: WAV-Dateien und Konfiguration werden von einer FAT-formatierten SD-Karte gelesen.
- **GPIO-Events**: Bis zu 10 Eingangspins lösen Soundwiedergaben aus (direkt oder binär-codiert für Williams System 11 / Gottlieb System 80).
- **Serielle Steuerung**: Sounds können über UART oder I2C (Slave) ausgelöst werden.
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
| `evt` | `none`, `flat`, `flat0`, `bw11`, `bg80` | `flat` | GPIO-Eventmodus |
| `deb` | Zahl (ms) | `5` | Entprellzeit in Millisekunden |
| `rpd` | Zahl (ms) | `60` | Ruheperiode in Millisekunden (nur `flat`) |
| `ser` | `none`, `uart`, `i2c` | `none` | Serielle Schnittstelle |
| `addr` | Hex-Zahl | `0x66` | I2C-Slave-Adresse (nur wenn `ser=i2c`) |

**Beispiel:**
```
dac=12
mix=div2
evt=flat
deb=10
rpd=40
ser=uart
```

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
| `bg80` | Gottlieb System 80/80A – binär codierter Sound-Bus |

Bei `flat` / `flat0`: Pin 1–10 lösen Sound-ID 1–10 aus. Eingänge sind Active-Low mit externem 10-kΩ-Pull-up.

**Entprellung und Ruheperiode (`flat`-Modus):**
- `deb`: Nach einer erkannten Flanke wird der Pin für diese Dauer gesperrt.
- `rpd`: Innerhalb dieser Zeit nach dem letzten Event wird ein weiteres Event auf demselben Pin ignoriert und die Zeitfenster vorgeschoben.

---

### Serielle Schnittstelle (`ser`)

UART und I2C verwenden dieselben GPIO-Pins und schließen sich gegenseitig aus.

#### UART (`ser=uart`)
- 115200 Baud, 8N1, kein Handshake
- Befehl zum Abspielen: `p <id>` gefolgt von Zeilenende
  Beispiel: `p 19` spielt Sound-Datei 19 ab

#### I2C Slave (`ser=i2c`)
- Adresse konfigurierbar über `addr=` (Standard: `0x66`)
- 7-Bit-Adressierung

| Befehl (Byte 0) | Byte 1 | Funktion |
|-----------------|--------|----------|
| `2` | – | Echo: Antwortet mit empfangenem Datensatz |
| `3` | – | Gibt Board-ID und Firmware-Version zurück (7 Bytes) |
| `20` | Sound-ID | Spielt Sound mit angegebener ID ab |

---

## Benennung von Sound-Dateien

Sound-Dateien liegen im Wurzelverzeichnis der SD-Karte und folgen diesem Namensschema:

```
NNNN-AAAA-VVV-beschreibung.wav
```

| Feld | Beschreibung |
|------|--------------|
| `NNNN` | 4-stellige numerische ID (z. B. `0012`) |
| `AAAA` | 4 Attributzeichen (s. Tabelle unten) |
| `VVV` | Lautstärke 0–100 |
| `beschreibung` | Beliebiger Textname (kann Bindestriche enthalten) |

**Beispiel:** `0012-xbxx-100-ring-my-bell.wav`

### Attribute (Position 1–4)

| Position | Zeichen | Bedeutung |
|----------|---------|-----------|
| 1 | `l` | Loop – Datei wird endlos wiederholt |
| 1 | `x` | Kein Loop |
| 2 | `b` | Break – laufende Instanz desselben Sounds wird gestoppt, bevor neu gestartet wird |
| 2 | `x` | Kein Break |
| 3 | `i` | Init/Hintergrund – wird beim Start automatisch abgespielt; wenn auch `l` (Pos. 1) gesetzt, als Hintergrundmusik-Loop |
| 3 | `x` | Kein Init-Sound |
| 4 | `k` | Kill – alle laufenden Tracks werden gestoppt, bevor dieser Sound startet |
| 4 | `c` | Soft-Kill – alle Tracks außer Init/Hintergrund werden gestoppt |
| 4 | `x` | Kein Kill |

---

## Soundgruppen

Gruppen fassen mehrere Sounds zusammen. Beim Aufruf der Gruppen-ID wird ein Mitglied ausgewählt und abgespielt.

Gruppen-Dateien liegen ebenfalls im Wurzelverzeichnis der SD-Karte:

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
