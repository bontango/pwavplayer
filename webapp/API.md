# PWAVplayer Config-Editor API

This document describes the two transports the PWAVplayer Config Editor
(`webapp/PWAVplayer_config_editor.html`) uses to talk to the device:

1. **USB serial** — a frame-based text protocol on `UART_NUM_0`, implemented in
   `main/usbserial.c`. Baud rate is configurable via `usbbaud` in `config.txt`
   (default `115200`; also supported: `230400`, `460800`, `921600`). Editor and firmware
   must agree on the baud.
2. **HTTP REST** — an `esp_http_server` on TCP port **8080**, implemented in
   `main/httpserver.c`. Started only when `wifi_enable=yes` **and** the STA connection
   produced an IP. Runs in parallel to USB — the editor chooses one transport per session.

Both transports share the same SD-card helpers (`main/cmdapi.c`) — no SD logic is
duplicated.

---

## Protocol Overview

All messages are newline-terminated (`\r\n`). The protocol is request/response with optional
data frames for bulk transfers.

### Handshake

On connection, the editor sends a single `0x55` byte. The firmware must respond:

```
RX: 0x55
TX: OK:READY\r\n
```

If the firmware is not ready or the byte is received mid-stream, the editor will connect anyway
after a 3-second timeout.

### Response Format

Every command response begins with `OK:` on success or `ERR:` on failure:

```
TX: OK:<optional message>\r\n
TX: ERR:<error message>\r\n
```

### Data Frames (text)

Used for text transfers (file lists, config.txt content as text):

```
Device TX:  DATA:BEGIN=<byte_size>\r\n
            <line1>\r\n
            <line2>\r\n
            ...
            DATA:END\r\n

Editor TX (every 8 lines received):  OK:CHUNK_ACK\r\n
Editor TX (after DATA:END):          OK:DATA_RECEIVED\r\n
```

### Binary Frames (base64)

Used for binary file transfers (WAV files, firmware binaries, config.txt as binary):

```
Device TX:  BINDATA:BEGIN=<byte_count>\r\n
            <base64_chunk1>\r\n          (144 raw bytes → 192 base64 chars)
            <base64_chunk2>\r\n
            ...
            BINDATA:END\r\n

Editor TX (every 8 lines received):  OK:CHUNK_ACK\r\n
Editor TX (after BINDATA:END):       OK:BINDATA_RECEIVED\r\n
```

The same frame formats are used in **both directions** (editor→device and device→editor).

---

## Commands

### 1. Handshake

**Purpose:** Verify communication and signal readiness.

```
Editor TX:  0x55  (single byte)
Device TX:  OK:READY\r\n
```

**Firmware:** Check for `0x55` in UART receive buffer; respond immediately.

---

### 2. FILE:DOWNLOAD=\<filename\>

**Purpose:** Download a file from the SD card to the editor.

```
Editor TX:  FILE:DOWNLOAD=config.txt\r\n
Device TX:  BINDATA:BEGIN=<size>\r\n
            <base64 encoded file content>
            BINDATA:END\r\n
```

**Firmware implementation:**
- Open `/<filename>` on SD card (SPIFFS/FAT mount point)
- Read all bytes, base64-encode in 144-byte chunks
- Send as BINDATA frame
- If file not found: `ERR:FILE_NOT_FOUND\r\n`

---

### 3. FILE:UPLOAD=\<filename\>

**Purpose:** Upload a file from the editor to the SD card.

```
Editor TX:  FILE:UPLOAD=config.txt\r\n
Editor TX:  BINDATA:BEGIN=<size>\r\n
            <base64 encoded file content>
            BINDATA:END\r\n
Device TX:  OK:FILE_SAVED\r\n
```

**Firmware implementation:**
- Wait for BINDATA frame after receiving this command
- Decode base64, write bytes to `/<filename>` on SD card
- Acknowledge every 8 chunks with `OK:CHUNK_ACK\r\n`
- Respond `OK:FILE_SAVED\r\n` after `BINDATA:END`
- On write error: `ERR:WRITE_FAILED\r\n`

**Note:** `config.txt` changes take effect on next boot (firmware calls `ReadConfig()` only in `app_main()`).

---

### 4. FILE:LIST

**Purpose:** List all files in the SD card root directory.

```
Editor TX:  FILE:LIST\r\n
Device TX:  DATA:BEGIN=<size>\r\n
            {"files":[{"name":"0001.wav","size":44100,"mtime":1743600000},{"name":"config.txt","size":64,"mtime":1743600060}]}\r\n
            DATA:END\r\n
```

**Firmware implementation:**
- Open SD card root directory
- Iterate all files (skip subdirectories)
- Use `stat()` to obtain `st_size` and `st_mtime` (Unix seconds) for each file
- Build JSON array with `name`, `size` (bytes), and `mtime` (Unix timestamp) for each file
- Send as DATA text frame
- If SD card not mounted: `ERR:SD_NOT_MOUNTED\r\n`

**Note:** `mtime` is only meaningful if the device clock has been set via `SET:TIME` before
the files were written. The editor sends `SET:TIME` automatically after connecting.

**Fallback format** (if JSON is inconvenient): one file per line, tab-separated name and size:
```
0001.wav\t44100\r\n
config.txt\t64\r\n
```

---

### 5. FILE:DELETE=\<filename\>

**Purpose:** Delete a file from the SD card root directory.

```
Editor TX:  FILE:DELETE=0001.wav\r\n
Device TX:  OK:FILE_DELETED\r\n
            (or ERR:DELETE_FAILED\r\n / ERR:INVALID_FILENAME\r\n)
```

**Firmware implementation:**
- Validate filename with `cmdapi_resolve_path()` (rejects `..`, `/`, `\`)
- Call `remove(fpath)` to delete the file
- Respond `OK:FILE_DELETED` on success, `ERR:DELETE_FAILED` on error

---

### 6. FILE:RENAME=\<old\>,\<new\>

**Purpose:** Rename a file on the SD card.

```
Editor TX:  FILE:RENAME=old.wav,new.wav\r\n
Device TX:  OK:FILE_RENAMED\r\n
            (or ERR:RENAME_FAILED\r\n / ERR:INVALID_ARGS\r\n / ERR:INVALID_FILENAME\r\n)
```

**Firmware implementation:**
- Parse `<old>` and `<new>` at the comma separator
- Validate both names with `cmdapi_resolve_path()`
- Call `rename(old_path, new_path)`
- Respond `OK:FILE_RENAMED` on success, `ERR:RENAME_FAILED` on error
- Respond `ERR:INVALID_ARGS` if no comma is present

---

### 7. SET:TIME=\<unix_seconds\>

**Purpose:** Synchronise the ESP32 system clock so that file uploads receive correct FAT
timestamps. The editor sends this command automatically after a successful handshake.

```
Editor TX:  SET:TIME=1743600000\r\n
Device TX:  OK:TIME_SET\r\n
```

**Firmware implementation:**
- Parse the decimal Unix timestamp from the argument
- Call `settimeofday()` with the value
- Respond `OK:TIME_SET\r\n`
- If the value is ≤ 0, do nothing but still respond `OK:TIME_SET\r\n`

---

### 8. WIFI:STATUS

**Purpose:** Query the current WiFi connection state. Intended for the editor's
**Device → Get IP Status** button so a user can find out which IP the device got before
switching to IP mode.

```
Editor TX:  WIFI:STATUS\r\n
Device TX:  OK:WIFI_STATUS=<state>[,ip=<ip>][,ssid=<ssid>]\r\n
```

**States:**

| `<state>` | Meaning |
|-----------|---------|
| `disabled` | `wifi_enable=no` in `config.txt` |
| `no_ssid` | `wifi_enable=yes` but `wifi_ssid` is empty |
| `disconnected` | Enabled + SSID set, but not currently associated (connect failed or link dropped) |
| `connected` | Associated with an AP; `ip=` and `ssid=` fields are included |

**Examples:**
```
OK:WIFI_STATUS=disabled
OK:WIFI_STATUS=no_ssid
OK:WIFI_STATUS=disconnected,ssid=MyNetwork
OK:WIFI_STATUS=connected,ip=192.168.1.42,ssid=MyNetwork
```

---

### 9. REBOOT

**Purpose:** Trigger a software reset of the ESP32.

```
Editor TX:  REBOOT\r\n
Device TX:  OK:REBOOTING\r\n
            (device restarts)
```

**Firmware implementation:**
- Send `OK:REBOOTING\r\n`
- Call `esp_restart()` after a short delay (~100ms) to allow the response to be transmitted

---

## Implementation Notes

### Implementation in `main/usbserial.c`

The API is fully implemented in `main/usbserial.c` as the `UsbSerial` FreeRTOS task.
The task runs on `UART_NUM_0` (the ESP32 USB-CDC / JTAG UART) and is independent of the
sound-command serial interface (`cserial.c`). The baud rate is read from the global
`gusbbaud` (set by `ReadConfig()` from the `usbbaud` key in `config.txt`, default `115200`).

### SD Card Mount Point

The SD card is mounted in `main/main.c`. The mount point is exported as `mount_point[]`
from `wavplayer.c` and used by all `cmdapi_*` functions in `cmdapi.c`.

### Task/Buffer Considerations

- File transfers can be large (WAV files up to several MB)
- `usbserial.c` uses streaming reads/writes — files are never fully loaded into RAM
- RX buffer: 4096 bytes; line buffer: 256 bytes; base64 decode buffer: 200 bytes

### UART Port

Current config: `UART_NUM_0`, baud rate from `gusbbaud` (default `115200`, configurable via
`usbbaud=` in `config.txt`). No dedicated TX/RX GPIOs — uses the built-in USB serial
converter. See `usbserial.c` (`USB_UART_PORT`, `gusbbaud`).

### Enabling the API

The `UsbSerial` task is started unconditionally from `main/main.c` alongside the other
tasks, regardless of the `ser=` setting in `config.txt`.

---

## Summary Table

| Command | Direction | Data Format | Response |
|---------|-----------|------------|----------|
| `0x55` (byte) | Editor→Device | — | `OK:READY` |
| `FILE:DOWNLOAD=<name>` | Editor→Device | — | BINDATA frame |
| `FILE:UPLOAD=<name>` | Editor→Device | followed by BINDATA frame | `OK:FILE_SAVED` |
| `FILE:LIST` | Editor→Device | — | DATA frame (JSON) |
| `FILE:DELETE=<name>` | Editor→Device | — | `OK:FILE_DELETED` |
| `FILE:RENAME=<old>,<new>` | Editor→Device | — | `OK:FILE_RENAMED` |
| `SET:TIME=<unix_seconds>` | Editor→Device | — | `OK:TIME_SET` |
| `WIFI:STATUS` | Editor→Device | — | `OK:WIFI_STATUS=<state>[,ip=,ssid=]` |
| `REBOOT` | Editor→Device | — | `OK:REBOOTING` |

---

## HTTP REST API (port 8080)

Implemented in `main/httpserver.c`. Started from `main/main.c` after `wifi_init_sta()`
succeeds. All responses carry `Access-Control-Allow-Origin: *` so the browser editor can
call them from a local file.

- **Base URL:** `http://<device-ip>:8080`
- **API version:** `1` (see `HTTP_API_VERSION` in `main/httpserver.h`). The editor reads
  `api_version` from `/status` and warns on mismatch.
- **Auth / TLS:** none — the server is designed for a trusted LAN (same trust model as
  LISYclock).

### Endpoints

| Method | Route | Request body | Response | Description |
|--------|-------|--------------|----------|-------------|
| GET    | `/status` | — | `{"status":"ok","version":"<fw>","api_version":1}` | Health + firmware version |
| GET    | `/config` | — | `text/plain` | Streams `config.txt` from SD |
| POST   | `/config` | `text/plain` body | `OK` | Writes body to `/sdcard/config.txt` |
| GET    | `/files` | — | JSON array: `[{"name":"...","size":N,"mtime":T}, ...]` | SD root listing |
| GET    | `/files/<name>` | — | `application/octet-stream` | Downloads the named file |
| PUT    | `/files/<name>` | binary body | `OK` | Uploads / overwrites the file |
| DELETE | `/files/<name>` | — | `OK` | Deletes the file |
| POST   | `/rename` | `{"old_name":"a","new_name":"b"}` | `OK` | Renames a file on SD |
| POST   | `/reboot` | `{"confirm":"reboot"}` | `OK` | Triggers `esp_restart()` |
| POST   | `/update` | binary body (.bin) | `OK` (then reboot) | Uploads firmware to `update.bin`; device flashes it on next boot |
| POST   | `/play`   | `{"id":<num>}` | `OK` | Enqueues `{cmd='p', arg=id}` on the `xpinevt` stream buffer — same as USB `p <num>` |
| OPTIONS | `/*`     | — | `204 No Content` | CORS preflight |

### Path-traversal protection

`/files/<name>` routes reuse `cmdapi_resolve_path()`, which rejects `..`, `/` and `\`
in the filename component. The same helper is used by the USB file handlers.

### Progress

The editor uses `XMLHttpRequest` for `/files/<name>` (GET and PUT) and `/update` so that
`upload.onprogress` / `onprogress` events drive the progress bars. `fetch()` does not
expose upload progress on current browsers.
