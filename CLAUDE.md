# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**pwavplayer** is a polyphonic WAV file player for ESP32 microcontrollers, designed for retro pinball machine sound systems. It plays multiple WAV files simultaneously from an SD card via a DAC, with GPIO-triggered events and optional serial (UART/I2C) control.

## Build System

This project uses **ESP-IDF 5.5.1** with CMake. Requires the IDF environment to be activated before running any commands.

```bash
idf.py build              # Build firmware
idf.py flash              # Flash to device (COM3, UART)
idf.py monitor            # Serial monitor at 115200 baud
idf.py build flash monitor # Combined build+flash+monitor
idf.py menuconfig         # SDK configuration menu
```

IDF path: `C:\Users\bonta\esp\v5.5.1\esp-idf`
Target: ESP32 (WROVER) or ESP32-S3 — selected via `platform.h`

There is no linting or test framework; this is bare-metal embedded firmware.

## Architecture

**Multi-tasked event-driven system using FreeRTOS:**

- **Core 0**: `WAVPlayer` task (priority 12) — main audio processing loop
- **Core 1**: Event handler task (priority 5–6) + Serial handler task (priority 5–6)

**Interprocess communication**: A `StreamBuffer` (`xpinevt`) carries `{cmd, arg}` structs from event handlers to the WAV player. The command `'p'` with a sound ID triggers playback.

### Key Components

| File | Role |
|------|------|
| `main/main.c` | Entry point: mounts SD card, reads `config.txt`, creates FreeRTOS tasks |
| `main/wavplayer.c` | Core audio engine: WAV parsing, mixing, FIFO buffer, SD card I/O |
| `main/pevent.c` | GPIO event handler with debouncing and interrupt management |
| `main/cserial.c` | UART or I2C serial command interface (mutually exclusive) |
| `main/fupdate.c` | OTA firmware update from `update.bin` on SD card at boot |
| `main/encevG80.c` | Gottlieb System 80/80A binary-encoded event decoder |
| `main/encevW11.c` | Williams System 11 event decoder (partial) |
| `main/pwav.h` | Shared config macros, IPC command struct, buffer size |
| `main/pgpio.h` | GPIO pin mappings for WROVER and S3 variants |
| `main/platform.h` | Platform selector (`#define ESP32_WROVER` or `ESP32_S3`) |

### Audio Engine (`wavplayer.c`)

- Manages multiple concurrent audio tracks as linked list nodes
- Reads WAV headers; expects PCM 8-bit or 16-bit mono/stereo files on SD
- Ring buffer (1024 samples) feeds the DAC via a timer interrupt
- Mixer modes: `sum`, `div2`, `sqrt` (configurable in `config.txt`)
- DAC output: 12-bit or 16-bit (configurable)

### Configuration (`config.txt` on SD card)

```
dac: 12          # DAC bit depth (12 or 16)
mix: sum         # Mixer mode: sum | div2 | sqrt
events: flat     # Event mode: none | flat | flat0 | bw11 | bg80
debounce: 20     # GPIO debounce in milliseconds
serial: uart     # Serial mode: none | uart | i2c
i2caddr: 0x08    # I2C address (hex, used only if serial=i2c)
rest: 500        # Rest period in milliseconds
```

### Event Modes

- `flat` / `flat0`: Direct GPIO pin → sound ID mapping (new vs. old wiring)
- `bw11`: Williams System 11 binary-encoded inputs → `encevW11.c`
- `bg80`: Gottlieb System 80/80A binary-encoded inputs → `encevG80.c`
