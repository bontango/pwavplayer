# PWAVplayer

A polyphonic WAV file player for ESP32 microcontrollers, designed for use in pinball machines. The device reads WAV files from an SD card and outputs them through a DAC — multiple tracks can be played and mixed simultaneously.

(C) 2025 colrhon.org – [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)

---

## Features

- **Polyphony**: Multiple WAV files are played simultaneously and mixed in real time.
- **SD card**: WAV files and configuration are read from a FAT-formatted SD card.
- **GPIO events**: Up to 10 input pins trigger sound playback (directly or binary-encoded for Williams System 11 / Gottlieb System 1/80/80A/80B).
- **Serial control**: Sounds can be triggered via UART or I2C (slave).
- **USB Config Editor**: Browser-based editor (`webapp/`) for managing the SD card and configuration over USB (Chrome/Edge).
- **WiFi / HTTP access (optional)**: The same editor can also talk to the device over WiFi (HTTP REST on port 8080), in parallel to USB. STA-mode only, credentials in `config.txt`.
- **File attributes**: Individual WAV files can be configured as loop, background sound, init sound, or with kill behavior.
- **Sound groups**: Multiple sounds can be combined into a group from which one is selected randomly or sequentially.
- **Firmware update**: New firmware can be placed as `update.bin` on the SD card and is automatically flashed on the next boot.

---

## Configuration

### config.txt

The file `config.txt` must be placed in the root directory of the SD card. Each line uses the format `key=value`. Missing entries fall back to default values.

| Key | Values | Default | Description |
|-----|--------|---------|-------------|
| `dac` | `12`, `16` | `12` | DAC output bit depth |
| `mix` | `sum`, `div2`, `sqrt` | `div2` | Mixing mode for multiple simultaneous tracks |
| `evt` | `none`, `flat`, `flat0`, `bw11`, `bg80`, `by35` | `bg80` | GPIO event mode |
| `deb` | number (ms) | `10` | Debounce time in milliseconds |
| `rpd` | number (ms) | `60` | Rest period in milliseconds (`flat` mode only) |
| `ser` | `none`, `uart`, `i2c` | `none` | Serial interface for sound commands |
| `addr` | hex value | `0x66` | I2C slave address (only when `ser=i2c`) |
| `usbbaud` | `115200`, `230400`, `460800`, `921600` | `115200` | Baud rate of the USB config-editor port (UART0) |
| `wifi_enable` | `yes`, `no` | `no` | Enables STA-mode WiFi and the HTTP server on port 8080 |
| `wifi_ssid` | string | *(empty)* | WiFi network SSID — required if `wifi_enable=yes` |
| `wifi_pwd` | string | *(empty)* | WiFi password |

**Example:**
```
dac=12
mix=div2
evt=bg80
deb=10
rpd=40
ser=uart
usbbaud=921600
wifi_enable=yes
wifi_ssid=MyNetwork
wifi_pwd=secret
```

> **Note:** If `config.txt` is missing, the firmware starts with the default values above.
> The `usbbaud` default of `115200` keeps `idf.py monitor` working out of the box.
> Raise it (e.g. `921600`) for faster file transfers with the config editor — then match
> the editor's "Baud" drop-down to the same value before reconnecting.

---

### Mixing modes (`mix`)

| Value | Behavior |
|-------|----------|
| `sum` | All track samples are added together (may clip) |
| `div2` | Each sample is halved before adding |
| `sqrt` | Samples are summed and divided by the square root of the track count |

---

### GPIO event mode (`evt`)

| Value | Description |
|-------|-------------|
| `none` | No GPIO events |
| `flat` | 10 input pins, each pin triggers the sound with the same number (from rel. 0.9.2, with rest period) |
| `flat0` | Like `flat`, older variant without rest period (up to rel. 0.9.1) |
| `bw11` | Williams System 11 – binary-encoded sound bus |
| `bg80` | Gottlieb System 1/80/80A/80B – binary-encoded sound bus |
| `by35` | Bally -35 / Stern MPU – binary-encoded sound bus, read on external strobe (GPIO34, positive edge) |

For `flat` / `flat0`: pins 1–10 trigger sound IDs 1–10. Inputs are active-low with an external 10 kΩ pull-up resistor.

**Debounce and rest period (`flat` mode):**
- `deb`: After a detected edge, the pin is locked for this duration.
- `rpd`: Within this time after the last event, a further event on the same pin is ignored and the time window is extended.

---

### Serial interface (`ser`)

UART and I2C share the same GPIO pins and are mutually exclusive.

#### UART (`ser=uart`)
- 115200 baud, 8N1, no handshake
- Playback command: `p <id>` followed by a newline
  Example: `p 19` plays sound file 19

#### I2C Slave (`ser=i2c`)
- Address configurable via `addr=` (default: `0x66`)
- 7-bit addressing

| Command (byte 0) | Byte 1 | Function |
|------------------|--------|----------|
| `2` | – | Echo: replies with the received data |
| `3` | – | Returns board ID and firmware version (7 bytes) |
| `20` | Sound ID | Plays the sound with the given ID |

---

## Sound File Naming

Sound files are placed in the root directory of the SD card and follow this naming scheme:

```
NNNN-AAAA-VVV-description.wav
```

| Field | Description |
|-------|-------------|
| `NNNN` | 4-digit numeric ID (e.g. `0012`) |
| `AAAA` | 4 attribute characters (see table below) |
| `VVV` | Volume 0–100 |
| `description` | Any text label (may contain hyphens) |

**Example:** `0012-xbxx-100-ring-my-bell.wav`

### Attributes (positions 1–4)

| Position | Character | Meaning |
|----------|-----------|---------|
| 1 | `l` | Loop – file repeats indefinitely |
| 1 | `x` | No loop |
| 2 | `b` | Break – a running instance of the same sound is stopped before restarting |
| 2 | `x` | No break |
| 3 | `i` | Init/background – played automatically on startup; if `l` (pos. 1) is also set, acts as a background music loop |
| 3 | `x` | No init sound |
| 4 | `k` | Kill – all running tracks are stopped before this sound starts |
| 4 | `c` | Soft-kill – all tracks except init/background are stopped |
| 4 | `x` | No kill |

---

## Sound Groups

Groups combine multiple sounds. When a group ID is triggered, one member is selected and played.

Group files are also placed in the root directory of the SD card:

```
NNNN-A-M1-M2-M3-description.grp
```

| Field | Description |
|-------|-------------|
| `NNNN` | 4-digit group ID |
| `A` | Selection mode: `m` = random, `r` = sequential (round-robin) |
| `M1`, `M2`, … | Sound IDs of the group members |

**Example:** `0009-m-15-71-12-exit-lane-left.grp` – randomly selects from sounds 15, 71, and 12.

---

## Spoken Version Number

On startup, a spoken version announcement is played automatically if the matching file is present:

```
/spokenvers/version-X-Y-Z.wav
```

where X, Y, Z are the three parts of the version number (e.g. `version-0-9-3.wav` for version 0.9.3).

---

## Firmware Update

New firmware can be copied to the SD card:

```
update.bin
```

On the next boot, the device checks whether the file is present and valid, flashes it into the inactive OTA partition, and restarts with the new version. The file is not automatically deleted after a successful update.

---

## Config Editor (USB or WiFi)

The directory `webapp/` contains `PWAVplayer_config_editor.html` — a browser-based editor for Chrome or Edge (desktop). It can talk to the device over **USB** (Web Serial) or over **WiFi** (HTTP REST on port 8080). The mode is chosen in the Device tab; both transports run in parallel on the firmware side.

### Features

- **Configuration**: Edit `config.txt`, upload it to the device or download it (incl. WiFi settings in the *General* tab)
- **SD card**: View file list (name, size, date), download, rename, and delete files
- **Firmware update**: Transfer a local `.bin` file as `update.bin` to the SD card; full flash utility via esptool-js (browser USB)
- **Default configuration**: Load a default `config.txt` from lisy.dev
- **WiFi status (USB)**: The *Device* tab has a **Get IP Status** button that queries the device over USB and copies the assigned IP into the IP-mode address field
- **Debug tab**: Low-level USB protocol log, moved out of the Device tab

### Requirements

- Chrome or Edge (desktop) — Web Serial API required for USB mode
- ESP32 connected via USB, or reachable on the LAN (WiFi STA)
- Firmware running (`UsbSerial` task is always active; HTTP server starts only when `wifi_enable=yes` and the device got an IP)

### Protocols

- **USB**: frame-based text protocol on `UART_NUM_0`; baud rate from `usbbaud` (default `115200`).
- **HTTP**: REST on port 8080, CORS-enabled, no authentication (designed for a trusted LAN).

See `webapp/API.md` for wire-level details of both transports and `webapp/README.md` for
operating instructions.
