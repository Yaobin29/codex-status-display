# Codex Status Display

A small ESP32 + ST7789 desk display for local Codex chat status.

It shows the few signals that matter when Codex is working in the background:

- `Run`: active Codex chats.
- `Done`: chats completed today, reset at local midnight.
- `Active`: running chats, with response-needed chats marked as orange `RESP`.

![Codex status display](docs/assets/codex-status-display-thumb.png)

This is a personal maker project, not an official OpenAI or Codex product. It is local-first: your Codex state stays on your machine, and the board receives only a compact status summary.

## How It Works

```text
Codex local state -> Python bridge -> compact JSON -> USB serial or WiFi HTTP -> ESP32 screen
```

The Python bridge reads local Codex state from `~/.codex/state_5.sqlite` and Codex session JSONL logs. It does not read private Codex app cache or IndexedDB state.

The ESP32 firmware can receive the same compact JSON payload in two ways:

- USB serial: useful for first bring-up and debugging.
- WiFi HTTP polling: useful for daily desk use.

## Repository Layout

```text
.
|-- codex_status_display.py
|-- firmware/
|   |-- esp32_st7789_serial/
|   `-- esp32c6_st7789_status/
|-- scripts/
|-- tests/
`-- docs/
```

## Host Bridge

Run once and print the compact JSON payload:

```bash
python3 codex_status_display.py --root /path/to/your/Codex_or_Avatar_node
```

Serve the payload over WiFi/LAN:

```bash
python3 codex_status_display.py --root /path/to/your/Codex_or_Avatar_node --http --watch --interval 5
```

Write one payload over USB serial:

```bash
python3 codex_status_display.py --root /path/to/your/Codex_or_Avatar_node --serial /dev/cu.usbmodem111201
```

You can also set `CODEX_STATUS_ROOT` and use the wrapper:

```bash
CODEX_STATUS_ROOT=/path/to/your/Codex_or_Avatar_node scripts/start_codex_status_display.sh --http --watch --interval 5
```

Runtime state is written under:

```text
local-runtime/codex-status-display/
```

That folder is intentionally ignored by git.

## Firmware Variants

| Board | Firmware directory | FQBN | Verified port | Display | Pins |
| --- | --- | --- | --- | --- | --- |
| Legacy ESP32/ST7789 | `firmware/esp32_st7789_serial/` | `esp32:esp32:esp32` | `/dev/cu.wchusbserial11120` | 135x240 ST7789 | CS 15, RST 4, DC 2 |
| ESP32-C6 MagnoDLP ST7789 | `firmware/esp32c6_st7789_status/` | `esp32:esp32:esp32c6` | `/dev/cu.usbmodem111201` | 172x320 ST7789 LCD | SCLK 7, MOSI 6, CS 14, DC 15, RST 21, BL 22 |

The ESP32-C6 LCD firmware uses dirty-region refresh: the board still polls the bridge every 5 seconds, but unchanged fields are left untouched so the LCD does not flash like a full-screen video refresh. It also drives the built-in RGB LED with a breathing/color-shift status effect.

## Arduino Build

Legacy ESP32/ST7789:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/esp32_st7789_serial
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/cu.wchusbserial11120 firmware/esp32_st7789_serial
```

ESP32-C6 MagnoDLP ST7789:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c6 firmware/esp32c6_st7789_status
arduino-cli upload --fqbn esp32:esp32:esp32c6 --port /dev/cu.usbmodem111201 firmware/esp32c6_st7789_status
```

Required Arduino libraries:

- `Adafruit_GFX`
- `Adafruit_ST7789`
- `ArduinoJson`

## WiFi Mode

USB-only mode needs no WiFi file.

For WiFi mode, copy the matching example file:

```text
firmware/<variant>/wifi_config.example.h
```

to:

```text
firmware/<variant>/wifi_config.h
```

Then fill in your local SSID, password, and bridge URL:

```cpp
#define CODEX_STATUS_URL "http://YOUR_MAC_LAN_IP:8787/wire"
```

The real `wifi_config.h` is ignored by git and should not be committed.

## Board Change Checklist

1. List serial ports with `ls /dev/cu.*`.
2. Identify the board with `arduino-cli board list`.
3. Pick the matching firmware variant and FQBN from the table.
4. Compile before upload.
5. Upload using the detected port.
6. Push one serial frame for immediate display.
7. Start the HTTP bridge and confirm `/wire` returns compact JSON.

## Project Skill

The local recovery and board-variant skill is mirrored at:

```text
docs/codex-status-display-service-skill.md
```

It records the current ESP32-C6 LCD board, the earlier legacy ESP32/ST7789 board, compile/upload recipes, WiFi-vs-serial behavior, and the recovery workflow.

## Tests

```bash
python3 -m unittest discover -s tests
```
