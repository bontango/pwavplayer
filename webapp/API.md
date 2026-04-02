# PWAVplayer USB Serial API

This document defines the USB serial API that must be implemented in the pwavplayer firmware
(`main/cserial.c` or a new `main/usbapi.c`) to support the PWAVplayer Config Editor web app.

The editor communicates via the **Web Serial API** (Chrome/Edge) using a frame-based text protocol
over UART (115200 baud by default).

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
            {"files":[{"name":"0001.wav","size":44100},{"name":"config.txt","size":64}]}\r\n
            DATA:END\r\n
```

**Firmware implementation:**
- Open SD card root directory
- Iterate all files (skip subdirectories)
- Build JSON array with `name` and `size` (bytes) for each file
- Send as DATA text frame
- If SD card not mounted: `ERR:SD_NOT_MOUNTED\r\n`

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

### 7. REBOOT

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

### Adding to `main/cserial.c`

The current UART handler (`SerialUART` task) only processes `p <num>` play commands.
Extend the command parser to handle the new commands:

```c
// In the UART receive loop, after reading a line:
if (strncmp(line, "FILE:DOWNLOAD=", 14) == 0) {
    handle_file_download(line + 14);
} else if (strncmp(line, "FILE:UPLOAD=", 12) == 0) {
    handle_file_upload(line + 12);
} else if (strcmp(line, "FILE:LIST") == 0) {
    handle_file_list();
} else if (strcmp(line, "REBOOT") == 0) {
    uart_write_bytes(UART_NUM_2, "OK:REBOOTING\r\n", 14);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
} else if (line[0] == 0x55) {
    uart_write_bytes(UART_NUM_2, "OK:READY\r\n", 10);
}
```

### SD Card Mount Point

The SD card is mounted in `main/main.c`. Use the same mount point (e.g. `/sdcard`) for all
file operations in the API handlers.

### Task/Buffer Considerations

- File transfers can be large (WAV files up to several MB)
- Use streaming reads/writes rather than loading entire files into RAM
- The UART task stack may need to be increased for large transfers
- Consider using a dedicated USB API task on Core 1 if UART task stack is insufficient

### UART Port

Current config: `UART_NUM_2`, GPIO 22 (TX), GPIO 21 (RX), 115200 baud.
See `main/cserial.c` and `main/pgpio.h` for pin definitions.

### Enabling the API

The USB serial API is only available when `ser=uart` is set in `config.txt`.
To make the API available regardless of `ser` setting, consider adding a dedicated
USB-serial task that always runs (separate from the I2C/UART sound command interface).

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
| `REBOOT` | Editor→Device | — | `OK:REBOOTING` |
