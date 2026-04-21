# PWAVplayer Config Editor — User Guide

`PWAVplayer_config_editor.html` is a single-file, browser-based tool for configuring a
PWAVplayer device, managing files on its SD card, and flashing firmware — all over USB
from a desktop computer. No installation required.

---

## Requirements

- **Browser:** Chrome or Edge (desktop). The editor relies on the
  [Web Serial API](https://developer.mozilla.org/docs/Web/API/Web_Serial_API), which is
  not available in Firefox or Safari.
- **Connection:** Device connected via USB (UART0 / built-in USB-Serial).
- **Firmware:** PWAVplayer firmware running on the device. The `UsbSerial` task is always
  active — no configuration needed to enable it.

---

## Launching the editor

Just open the HTML file in Chrome/Edge. The page works locally — no server or network
connection is required (except for optional downloads from `lisy.dev`, see below).

You can also host the file on any static web server and open it from there.

---

## Tabs

### 1. Device

Connect, monitor, and control the device.

- **Connect USB** — opens the browser's serial port chooser. Pick the ESP32 device and
  click *Connect*. After a short handshake the device responds with `OK:READY`.
- **Baud drop-down** — must match the firmware's `usbbaud` setting (default `115200`).
  Available: `9600`, `57600`, `115200`, `230400`, `460800`, `921600`. If the editor hangs
  on *Connecting…* for more than a few seconds, the two sides disagree about the baud rate.
- **USB console** — low-level log of bytes exchanged with the device. Useful for
  troubleshooting.
- **Disconnect** — closes the serial port. Do this before reflashing or sharing the port
  with another tool such as `idf.py monitor`.

### 2. General

Edits `config.txt` line by line via form fields. The table below lists the keys; defaults
match the firmware's `InitConfig()`.

| Key | Values | Default | Purpose |
|-----|--------|---------|---------|
| `dac` | `12`, `16` | `12` | DAC output bit depth |
| `mix` | `sum`, `div2`, `sqrt` | `div2` | Track mixing algorithm |
| `evt` | `none`, `flat`, `flat0`, `bw11`, `bg80`, `by35` | `bg80` | GPIO event mode |
| `deb` | milliseconds | `10` | Debounce time (GPIO edge lockout) |
| `rpd` | milliseconds | `60` | Rest period — `flat` mode only |
| `ser` | `none`, `uart`, `i2c` | `none` | Sound-command serial interface |
| `addr` | hex | `0x66` | I2C slave address, only when `ser=i2c` |
| `usbbaud` | `115200`…`921600` | `115200` | UART0 baud used by **this** editor |

**Workflow:**

1. *File → Open* to load a local `config.txt`, or *File → From device* to download the
   current one from the SD card.
2. Edit fields.
3. *File → Save to device* uploads the new `config.txt` and prompts you to reboot.
4. *File → Save as* saves a local copy.

> Changing `usbbaud` takes effect after reboot. Before you click *Connect USB* again, set
> the **Baud** drop-down on the Device tab to the same value.

### 3. Soundfiles

Manage the sound-file slots (IDs 0001–0031). The editor parses the
`NNNN-AAAA-VVV-description.wav` naming scheme and displays attributes (loop, break,
init/background, kill) in a table.

- **Refresh** — re-reads the SD file list.
- **Write Soundconfig** — renames files on the device so the filenames match the
  attributes you've set in the table.
- Per-row actions: **Assign**, play, download, rename, delete.

#### Assigning a WAV file to a sound slot

The **Assign** button in each row is the quickest way to put a new WAV file into a
specific slot (ID `0001`–`0031`). The editor takes care of naming and replacing any
existing file in that slot.

1. Connect the device (**Device → Connect USB**) and switch to the **Soundfiles** tab.
2. Click **Refresh** so the table reflects the current SD card contents.
3. In the row of the desired slot, click **Assign**. A file picker opens — choose the
   `.wav` file from your computer.
4. The editor derives the target filename from the slot ID and the source file's base
   name, e.g. picking `alarm.wav` for slot `0005` produces
   `0005-xxxx-xxx-alarm.wav` (attributes default to `x` / volume `100`; edit them later
   via *Write Soundconfig*).
5. If the slot already holds a different file, you are asked to confirm the
   replacement — the old file is deleted before the new one is uploaded. If the target
   name is identical, you are asked whether to overwrite.
6. The **Assign** button shows upload progress (`0%` → `100%`). On success a toast
   confirms the new filename and the table reloads automatically.
7. Adjust attributes (loop / break / init / background / kill / volume) in the table and
   click **Write Soundconfig** to rename the file accordingly.

> Tip: the description part of the filename is sanitized — only letters, digits and
> dashes are kept. Special characters and spaces are replaced with `-`.
>
> Tip: for many files at once it is faster to raise `usbbaud` first (see
> *Faster file transfers* below).

### 4. SD Card

Generic file manager for the SD card root.

- **File list** — name, size, modification time. The modification time is only meaningful
  if the device clock was set (the editor sends `SET:TIME=` automatically right after the
  handshake).
- **Upload / Download / Rename / Delete** — standard operations, streamed over the serial
  protocol described in `API.md`.
- **Init SD Card** — downloads a reference SD layout from `lisy.dev` and uploads it to the
  device. Useful for a fresh card. The *Overwrite existing* checkbox controls whether
  existing files are replaced.

### 5. Firmware / Flash

Two independent actions:

- **Fetch versions / Install server firmware** — downloads a firmware build from
  `lisy.dev` and places it on the SD card as `update.bin`. The device flashes it on the
  next boot via its built-in OTA path.
- **Flash ESP32 (browser)** — full-chip flash using
  [esptool-js](https://github.com/espressif/esptool-js). The browser opens a **separate**
  serial session at 115200 baud, puts the chip into bootloader mode, and writes the
  firmware binaries directly. Use this for first-time provisioning or recovery.

> The browser flasher holds the serial port exclusively. Disconnect the editor's USB
> session first, and don't run `idf.py monitor` at the same time.

---

## Typical first-time setup

1. Build and flash firmware once with `idf.py build flash` (or use the browser flasher).
2. Open the editor, click **Connect USB** (default baud `115200`).
3. Go to **SD Card → Init SD Card** to lay down the default file layout.
4. Switch to **General**, review settings, click **File → Save to device**.
5. Reboot the device. Done.

---

## Faster file transfers

For quick transfers of many WAV files, raise `usbbaud`:

1. **General tab** → set `usbbaud` to `921600` → **Save to device**.
2. **Reboot** the device (File → Reboot device).
3. **Device tab** → change the *Baud* drop-down to `921600`.
4. **Connect USB** again.

To go back, reverse the steps. If you end up with a mismatch and can no longer connect,
either:
- pick the correct baud in the editor, or
- delete `config.txt` on the SD card (via card reader) — the firmware will fall back to
  `115200`.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| *Connect USB* shows no devices | Another program holds the port (`idf.py monitor`, other editor tab, esptool) | Close the other session |
| Connects but stays *Connecting…* and times out | Baud mismatch between editor and firmware | Match the *Baud* drop-down to the firmware's `usbbaud` |
| `idf.py monitor` shows garbage after boot | `usbbaud` ≠ 115200 and monitor is at 115200 | Either set `usbbaud=115200`, or run `idf.py monitor -B <rate>` |
| File upload stalls | Weak USB cable or hub, very large file | Try a direct USB port; retry |
| Browser flasher fails | Wrong chip target or port busy | Verify the device resets into bootloader; disconnect other sessions |

---

## Related documents

- `API.md` — wire-level protocol specification (commands, frame formats).
- `../README.md` / `../LIESMICH.md` — project overview and firmware-side configuration.
