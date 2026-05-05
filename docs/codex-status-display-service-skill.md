---
name: codex-status-display-service
description: Start, stop, inspect, and recover the local Codex Status Display bridge that feeds the ESP32/ST7789 desk screen over WiFi or serial. Use this skill whenever the user says the Codex status screen, ESP32 display, single-chip screen, or board is disconnected after reboot, stale, not refreshing, or needs the service pulled up.
---

# Codex Status Display Service

Layer: `local-runtime`

Use this skill to recover the local host bridge for the Codex status display.

The ESP32 screen does not pull Codex state directly. It polls a Mac-side HTTP bridge:

```text
ESP32 screen -> WiFi -> http://<Mac LAN IP>:8787/wire -> codex_status_display.py -> local Codex state
```

After a Mac reboot, the ESP32 can still be powered and connected to WiFi, but the bridge process is gone unless it is started again.

## Problem Type

Usually `HOW`:

- Diagnose whether the bridge is running.
- Start or restart the local HTTP service.
- Confirm the board has a reachable `/wire` endpoint to poll.

## Quick Start

Run the helper:

```bash
project-skills/skills/codex-status-display-service/scripts/start_codex_status_display.sh
```

Useful commands:

```bash
project-skills/skills/codex-status-display-service/scripts/start_codex_status_display.sh status
project-skills/skills/codex-status-display-service/scripts/start_codex_status_display.sh restart
project-skills/skills/codex-status-display-service/scripts/start_codex_status_display.sh stop
project-skills/skills/codex-status-display-service/scripts/start_codex_status_display.sh logs
```

Defaults:

- HTTP bind: `0.0.0.0`
- HTTP port: `8787`
- Refresh interval: `5` seconds
- Local health URL: `http://127.0.0.1:8787/wire`
- Detached session name: `codex-status-display-http`
- Log file: `local-runtime/codex-status-display/service.log`

## Firmware Variants / Board Matrix

Use the board identity to choose the firmware directory, FQBN, and upload port.

| Board | Firmware directory | FQBN | Verified port | Display | Notes |
| --- | --- | --- | --- | --- | --- |
| Legacy ESP32/ST7789 | `services/codex-status-display/firmware/esp32_st7789_serial/` | `esp32:esp32:esp32` | `/dev/cu.wchusbserial11120` | 135x240 ST7789 | Original small status screen; CS 15, RST 4, DC 2. |
| ESP32-C6 MagnoDLP ST7789 | `services/codex-status-display/firmware/esp32c6_st7789_status/` | `esp32:esp32:esp32c6` | `/dev/cu.usbmodem111201` | 172x320 ST7789 LCD | Current C6 screen; SCLK 7, MOSI 6, MISO 5, CS 14, DC 15, RST 21, backlight 22. |

The ESP32-C6 variant uses the Adafruit ST7789 driver, local dirty-region LCD refresh, field-level updates for counts/time/list rows, and `RGB_BUILTIN` for board LED feedback. Do not switch it back to LVGL unless the user explicitly asks for a different UI stack.

## Board Notes

### Legacy ESP32/ST7789 135x240

This was the earlier small status screen, recovered from the existing local `online_weather` Arduino sketch.

- Firmware: `services/codex-status-display/firmware/esp32_st7789_serial/`
- Sketch: `esp32_st7789_serial.ino`
- Board profile used successfully: `ESP32 Dev Module`
- FQBN: `esp32:esp32:esp32`
- Historical upload port: `/dev/cu.wchusbserial11120`
- Serial baud: `115200`
- Display: ST7789, `135x240`, rotation `0`
- Driver stack: `Adafruit_GFX`, `Adafruit_ST7789`, `SPI`
- Pins:
  - `TFT_CS = 15`
  - `TFT_RST = 4`
  - `TFT_DC = 2`
- Data path:
  - USB serial newline JSON works without WiFi config.
  - WiFi polling works if a local `wifi_config.h` is present.
- UI behavior:
  - Original compact layout for small screen.
  - Older firmware may redraw larger regions than the C6 LCD version.

### ESP32-C6 MagnoDLP ST7789 172x320

This is the current larger LCD status screen, ported from the MagnoDLP ESP32-C6 LVGL board pinout but implemented with the Adafruit ST7789 driver.

- Firmware: `services/codex-status-display/firmware/esp32c6_st7789_status/`
- Sketch: `esp32c6_st7789_status.ino`
- Board profile used successfully: `ESP32C6 Dev Module`
- FQBN: `esp32:esp32:esp32c6`
- Verified upload port: `/dev/cu.usbmodem111201`
- Serial baud: `115200`
- Chip observed during upload: `ESP32-C6FH4`, USB Serial/JTAG
- Display: ST7789 LCD, `172x320`, rotation `0`
- Driver stack: `Adafruit_GFX`, `Adafruit_ST7789`, `SPI`
- Pins:
  - `TFT_SCLK = 7`
  - `TFT_MOSI = 6`
  - `TFT_MISO = 5` in the source reference; current Adafruit constructor uses MOSI/SCLK plus control pins
  - `TFT_CS = 14`
  - `TFT_DC = 15`
  - `TFT_RST = 21`
  - `TFT_BACKLIGHT = 22`
- Data path:
  - USB serial newline JSON works for debugging and immediate first-frame push.
  - WiFi polling works when local `wifi_config.h` is present.
- UI behavior:
  - Run/Done/Active LCD layout.
  - Local dirty-region refresh with field-level updates for time, counts, and list rows.
  - Incremental progress bar animation.
  - `RGB_BUILTIN` board LED feedback with breathing/color-shift status indication.

## Compile / Upload Recipes

Use the Arduino CLI installed with Arduino IDE. If `arduino-cli` is not on `PATH`, use:

```bash
/Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli
```

Legacy ESP32/ST7789:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 services/codex-status-display/firmware/esp32_st7789_serial
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/cu.wchusbserial11120 services/codex-status-display/firmware/esp32_st7789_serial
```

ESP32-C6 MagnoDLP ST7789:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c6 services/codex-status-display/firmware/esp32c6_st7789_status
arduino-cli upload --fqbn esp32:esp32:esp32c6 --port /dev/cu.usbmodem111201 services/codex-status-display/firmware/esp32c6_st7789_status
```

After upload, push one immediate status frame over USB serial so the screen does not need to wait for the next WiFi poll:

```bash
python3 services/codex-status-display/codex_status_display.py --root /Users/wyb/Documents/Codex_project/Robin_AIagent_v2 --serial /dev/cu.usbmodem111201 --quiet
```

For the legacy ESP32 board, use its detected serial port instead:

```bash
python3 services/codex-status-display/codex_status_display.py --root /Users/wyb/Documents/Codex_project/Robin_AIagent_v2 --serial /dev/cu.wchusbserial11120 --quiet
```

Always adjust `--serial` to the detected port when using a different board.

## WiFi vs Serial Behavior

- With no local `wifi_config.h`, firmware accepts newline-delimited JSON over USB serial at 115200 baud.
- With a local `wifi_config.h`, firmware connects to WiFi and polls `CODEX_STATUS_URL`, while still accepting USB serial JSON for debugging.
- The real `wifi_config.h` is local-only and ignored by git. Do not print, quote, or read its contents unless the user explicitly asks for firmware debugging.
- It is safe to check whether `wifi_config.h` exists; do not expose SSID, password, or local secrets.
- The Mac-side bridge must be running for WiFi mode. Current helper output should show `local_url=http://127.0.0.1:8787/wire`, a LAN `board_url`, and `interval_seconds=5`.

## When Port or Board Changes

1. List likely serial ports: `ls /dev/cu.usbmodem* /dev/cu.wchusbserial* 2>/dev/null`.
2. Identify the board with `arduino-cli board list` or esptool chip info.
3. Choose the firmware variant and FQBN from the board matrix above.
4. Compile first; fix compile errors before upload.
5. Upload with the detected port.
6. Push one status frame over serial for immediate display.
7. Confirm the bridge is running with the helper `status` command and check `local-runtime/codex-status-display/status.log.jsonl` for recent `http` or serial output.

## Recovery Workflow

When the user says the board is disconnected, stale, or not updating:

1. Run `status`.
2. If local `/wire` is down, run `start` or `restart`.
3. Confirm `http://127.0.0.1:8787/wire` returns compact JSON.
4. Report the Mac LAN URL shown by the helper, because the board firmware polls that address.
5. If the local endpoint works but the board is still stale, ask the user whether the board and Mac are on the same WiFi, then inspect the board-side `CODEX_STATUS_URL` only if firmware debugging is needed.

## Safety

- Do not print or read the real `wifi_config.h` unless the user explicitly asks for firmware debugging.
- Do not expose WiFi SSID/password, API keys, or local secrets in chat.
- Do not create or install a LaunchAgent automatically. If the user wants true boot-time autostart, explain that it is a separate persistent-login action and ask before creating it.

## Output Contract

When finished, report:

- whether the service was already running or newly started
- local health URL
- LAN URL for the board
- refresh interval
- log path
- any remaining blocker
