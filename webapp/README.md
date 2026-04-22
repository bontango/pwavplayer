# PWAVplayer Config Editor — User Guide

`PWAVplayer_config_editor.html` is a single-file, browser-based tool for configuring a
PWAVplayer device, managing files on its SD card, and flashing firmware — over **USB**
(Web Serial) or **WiFi** (HTTP REST on port 8080). No installation required.

---

## Requirements

- **Browser:** Chrome or Edge (desktop) for USB mode (Web Serial). Any modern browser
  works for IP mode.
- **Connection:** USB to the ESP32, **or** the device reachable on your LAN (WiFi STA).
- **Firmware:** PWAVplayer firmware running. The `UsbSerial` task is always active; the
  HTTP server starts only when `wifi_enable=yes` in `config.txt` and the device got an IP.

---

## Launching the editor

Just open the HTML file in Chrome/Edge. The page works locally — no server or network
connection is required (except for optional downloads from `lisy.dev`, see below).

You can also host the file on any static web server and open it from there.

---

## Tabs

### 1. Device

Connect, monitor, and control the device. The **Mode** radios at the top switch between
*USB Mode* and *IP Mode*; the selection is persisted in `localStorage`. Firmware-wise the
two transports run in parallel — the editor just picks one.

#### USB Mode

- **Connect USB** — opens the browser's serial port chooser. Pick the ESP32 device and
  click *Connect*. After a short handshake the device responds with `OK:READY`.
- **Baud drop-down** — must match the firmware's `usbbaud` setting (default `115200`).
  Available: `9600`, `57600`, `115200`, `230400`, `460800`, `921600`. If the editor hangs
  on *Connecting…* for more than a few seconds, the two sides disagree about the baud rate.
- **Disconnect** — closes the serial port. Do this before reflashing or sharing the port
  with another tool such as `idf.py monitor`.

#### IP Mode

- **IP Address** — enter the device's IP (no port, no scheme). The editor talks HTTP REST
  on port 8080.
- **Connect** — does `GET /status` with a 6-second timeout + one retry, shows the
  firmware version, and warns if the device's `api_version` doesn't match the editor's
  expected value.
- The status dot goes green on success, red on timeout/mismatch.

#### WiFi Status (USB only)

Under *Config Transfer* there is a dedicated **WiFi Status** block with a **Get IP
Status** button. It sends `WIFI:STATUS` over USB and reports one of: *disabled*,
*no_ssid*, *connection failed*, or *connected* with the assigned IP and SSID. When
connected, the IP is automatically copied into the *IP Address* field above, so you
can switch to IP Mode without typing.

### Debug tab

The low-level USB protocol log (bytes/lines exchanged) has its own tab, separate from
the Device tab. Useful for troubleshooting USB-mode issues. (IP-mode HTTP requests are
logged here too for consistency.)

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
| `wifi_enable` | `yes`, `no` | `no` | Enable WiFi STA + HTTP server on port 8080 |
| `wifi_ssid` | string | *(empty)* | WiFi SSID |
| `wifi_pwd` | string | *(empty)* | WiFi password |

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
- Per-row actions: **PC** (assign from PC), **SD** (assign from SD), play.

#### Assigning a WAV file to a sound slot

Each row has two assign buttons — **PC** uploads a new file from your computer,
**SD** picks an already-existing WAV file on the SD card and just renames it.
Both derive the target filename from the slot ID and the source file's base name
(attributes default to `x` / volume `100`; edit them later via *Write Soundconfig*).

**PC — upload from local disk**

1. Connect the device and switch to the **Soundfiles** tab.
2. Click **Refresh** so the table reflects the current SD card contents.
3. In the row of the desired slot, click **PC**. A file picker opens — choose the
   `.wav` file from your computer.
4. Example: picking `alarm.wav` for slot `0005` produces `0005-xxxx-100-alarm.wav`.
5. If the slot already holds a different file, you are asked to confirm the
   replacement — the old file is deleted before the new one is uploaded.
6. The button shows upload progress (`0%` → `100%`). On success a toast confirms
   the new filename and the table reloads automatically.

**SD — rename a file already on the card**

1. Make sure the target `.wav` file is already on the SD card (copied via card
   reader or previously uploaded).
2. Click **SD** in the desired row. A modal opens with all `.wav` files on the SD
   card; files that are already assigned to a slot are marked.
3. Pick a file and confirm. The editor renames it to `XXXX-xxxx-100-<desc>.wav`.
   No upload needed — this is instant.
4. If the slot already holds a different file, it is deleted first. If a file with
   the target name already exists, you are asked whether to overwrite it.

Finally, adjust attributes (loop / break / init / background / kill / volume) in
the table and click **Write Soundconfig** to rename the files accordingly.

> Tip: the description part of the filename is sanitized — only letters, digits and
> dashes are kept. Special characters and spaces are replaced with `-`.
>
> Tip: for many files at once it is faster to raise `usbbaud` first (see
> *Faster file transfers* below).

### 4. Soundgroups

Manage `.grp` files on the SD card. A sound group bundles several WAV IDs under one
trigger ID; when the group is triggered, one member is picked randomly (`m`) or in
round-robin order (`r`). See `../Groups.md` for the format.

- **Refresh** — re-reads the SD file list and picks out all `.grp` files.
- **Add Group** — appends a new (unsaved) group row with the next free ID.
- **Write Groups** — uploads new `.grp` files and renames existing ones so their
  filenames match the ID / mode / members / description fields in the table.

Per group:

- **ID** — 4-digit trigger ID (shares the namespace with sound IDs; don't reuse).
- **Mode** — `m` random or `r` round-robin.
- **Description** — free text; sanitized to letters/digits/hyphens on write.
- **Members** — expand the collapse to see every WAV currently on the SD card,
  tick the boxes of the sounds that should belong to this group. The summary line
  shows the resulting member-ID list; a second line previews the resulting
  filename (and the pending rename if it differs from the current one).
- **Delete** — removes the `.grp` file from the SD card (with confirmation).
  Deleting an unsaved new row just removes it locally.

Since a `.grp` file's content is ignored by the firmware, all state lives in the
filename — changing members, mode, ID, or description translates to a rename on
**Write Groups**.

### 5. SD Card

Generic file manager for the SD card root.

- **File list** — name, size, modification time. The modification time is only meaningful
  if the device clock was set (the editor sends `SET:TIME=` automatically right after the
  handshake).
- **Upload / Download / Rename / Delete** — standard operations, streamed over the serial
  protocol described in `API.md`.
- **Init SD Card** — downloads a reference SD layout from `lisy.dev` and uploads it to the
  device. Useful for a fresh card. The *Overwrite existing* checkbox controls whether
  existing files are replaced.

### 6. Firmware / Flash

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

- `API.md` — wire-level protocol specification for both USB frames and HTTP REST.
- `../README.md` / `../LIESMICH.md` — project overview and firmware-side configuration.
