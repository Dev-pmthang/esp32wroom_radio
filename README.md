# ESP32-WROOM-32 Retro Vinyl Web Radio & MP4 Video Streamer

Internet radio player, retro vinyl visualizer, and real-time Wi-Fi **MP4 Video Streamer (20–24 FPS)** for the **ESP32-WROOM-32** built with pure **ESP-IDF (v5.3+) & FreeRTOS** (no PSRAM required). Supports **Dual TFT Display Modes** (**ST7789 240×240** and **ST7735 128×160**), **MAX98357A I2S** pop-free audio playback, physical **BOOT (GPIO 0)** Play/Pause control, and an embedded **Web Control & Video Streaming Dashboard**.

---

## Wiring

### Mode 0: ST7789 1.54" IPS TFT Display (240×240 SPI)

| ST7789 Pin | ESP32-WROOM-32 | Note |
| :--- | :--- | :--- |
| **SCL (SCLK)** | `GPIO 18` | SPI2 Hardware Clock (27 MHz DMA) |
| **SDA (MOSI)** | `GPIO 23` | SPI2 Master Out Slave In |
| **RES (RST)** | `GPIO 4` | Hardware Reset |
| **DC** | `GPIO 2` | Data / Command select |
| **BLK** | `GPIO 32` | Backlight control (`HIGH` = ON) |
| **CS** | `GPIO 5` / `GND` | Chip Select (supports both CS-less & CS panels) |
| **VCC** | `3V3` | 3.3V power supply |
| **GND** | `GND` | Common ground |

### Mode 1: ST7735 1.8" TFT Display (128×160 SPI)

| ST7735 Pin | ESP32-WROOM-32 | Note |
| :--- | :--- | :--- |
| **SCK (CLK)** | `GPIO 18` | SPI2 Hardware Clock (20 MHz DMA) |
| **SDA (MOSI)** | `GPIO 23` | SPI2 Master Out Slave In |
| **CS** | `GPIO 15` | Chip Select |
| **A0 (DC)** | `GPIO 2` | Data / Command select |
| **RESET (RST)** | `GPIO 4` | Hardware Reset |
| **LED (BLK)** | `GPIO 32` | Backlight control (`HIGH` = ON) |
| **VCC** | `3V3` or `5V` | Display power supply |
| **GND** | `GND` | Common ground |

### MAX98357A I2S Digital Amplifier

| MAX98357A | ESP32-WROOM-32 | Note |
| :--- | :--- | :--- |
| **DIN** | `GPIO 25` | I2S Serial Audio Data |
| **BCLK** | `GPIO 26` | I2S Bit Clock |
| **LRC (LRCLK)** | `GPIO 27` | I2S Word / Left-Right Clock |
| **SD (MODE)** | `GPIO 33` | Hardware Shutdown / Pop-free Mute (`0` = Mute, `1` = Unmute) |
| **VIN** | `5V` or `3V3` | Amplifier power supply (5V recommended for 4Ω/3W speaker) |
| **GND** | `GND` | Common ground |

### Buttons

Each button connects between the GPIO and **GND** (internal pull-up is enabled in firmware, no external resistor needed).

| Button | ESP32-WROOM-32 | Action |
| :--- | :--- | :--- |
| **Play / Pause** | `GPIO 0` (`BOOT`) | Instant hardware mute (`GPIO 33 = 0`), pause/resume stream & vinyl rotation |

---

## Features & System Architecture

- **Dual TFT Display Mode & Crisp ST7735 Anti-Grain Optimization:**
  - Switchable between **Mode 0 (`ST7789 240×240`, `CS=GPIO 5`, `27 MHz`)** and **Mode 1 (`ST7735 128×160` / `160×128`, `CS=GPIO 15`, `20 MHz`)** via `RADIO_LCD_MODE` in `include/radio_config.h` or live from the **Web UI** (persisted in NVS).
  - **Zero-Skew & Anti-Grain ST7735 Init:** Keeps `CASET (0..127)` inside ST7735 GRAM bounds (`Offset 0,0`), configures `INVCTR (0xB4 = 0x07)` **Line Inversion** + calibrated `PWCTR`/`VMCTR1`/`Gamma`, and renders vinyl grooves using exact `R² ± R` integer circle bands to eliminate pixel grain.
- **20–24 FPS Wi-Fi MP4 Video Streaming (`WebSocket + ROM JPEG + Dual-Core Pipeline`):**
  - **JPEG over WebSocket (`/ws`):** Browser scales `.mp4` frames via `<canvas>` (`imageSmoothingQuality='medium'`) and compresses to JPEG (`~3–6 KB` at 72% quality — fitting inside a single lwIP `TCP_WND = 5,760 B` packet window without TCP ACK stalls).
  - **Wait-Free Dual-Core Triple-Buffering (`3 × 10 KB`):** Core 0 (`httpd`) receives WebSocket binary frames directly into static `.bss` slots and returns immediate ACKs (`< 1 ms`), while Core 1 (`ui_render_task` boosted to Priority `12`) decodes concurrently.
  - **ESP32 ROM `tjpgd` + Ping-Pong Async SPI DMA:** Decodes JPEG using zero-flash ESP32 ROM `tjpgd.h` while alternating strips (`s_bar_buf` split into dual `9.6 KB` halves) so CPU decode and SPI DMA overlap 100%.
  - **Browser Sliding Window (`MAX_INFLIGHT = 3`) & Wi-Fi Airtime Boost:** Uses `requestVideoFrameCallback` + `ws.send(blob)` zero-copy pipelining, pauses background radio HTTP/I2S streaming during video playback, and disables idle SoftAP beacons (`WIFI_MODE_STA`) when connected to a router.
- **Retro "Vinyl Player — Now Playing" UI:**
  - Warm analog cream canvas (`#F4F1EA`), rotating pitch-black vinyl record, terracotta center label (`#C24B38`), brass tonearm, live **Top-Left Wi-Fi IP Display**, **Scanline Dirty-Rectangle DMA Engine** (`19.2 KB` shared buffer), and **Fixed-Point Q10 Trigonometry (`>> 10`)**.
- **Pop-Free Instant Hardware Mute & Zero-Heap-Fragmentation Runtime:**
  - `GPIO 0` pulls `GPIO 33 (SD)` low in `< 1 µs` with dual-edge press/release bounce filtering; all state resides in static `.bss` (`g_radio_state`) synchronized via `<stdatomic.h>`, `EventGroup`, `StreamBuffer`, and `TaskNotify`.

### FreeRTOS Multi-Core Task Allocation

| Task Name | Core | Priority | Responsibility |
| :--- | :---: | :---: | :--- |
| `audio_i2s_task` | Core 1 | **16** | Reads pre-buffered audio from `StreamBuffer`, applies integer volume scaling, writes to I2S DMA |
| `button_ctrl_task` | Core 1 | **15** | Woken by `GPIO 0` `IRAM_ATTR` ISR; verifies press/release debounce & toggles hardware mute + Play/Pause |
| `wifi_and_web_task` | Core 0 | **7** | Manages Wi-Fi STA/AP (`WIFI_PS_NONE`), NVS credential recovery, and embedded `esp_http_server` |
| `http_stream_task` | Core 0 | **5** | Streams HTTP audio, parses in-stream `icy-metaint` (`StreamTitle`), feeds `16 KB` `StreamBuffer` |
| `ui_render_task` | Core 1 | **4** (`12` in Video) | Renders dirty rectangles or decodes WebSocket JPEG frames (`tjpgd` ROM + Ping-Pong SPI2 DMA) |

---

## Web UI & Wi-Fi Configuration

1. **Automatic STA Connection:** On boot, the ESP32 loads saved Wi-Fi credentials from NVS and prints/displays its local IP at the **top-left corner of the TFT screen** (e.g., `192.168.100.186`).
2. **Fallback Access Point:** If no saved Wi-Fi is found, connect to the hotspot **`ESP32-WebRadio-AP`** (password: `12345678`) and open `http://192.168.4.1/`.
3. **Web Dashboard (`http://<ESP32_IP>/`):**
   - View live station name, song title (`ICY StreamTitle`), and playback state.
   - Toggle **Play / Pause**, switch radio stations, and adjust **Volume (0–100%)**.
   - **Stream `.MP4` Video to TFT (`WS-JPEG`):** Pick any `.mp4` video file, choose scaling (**Full Screen** / **Fit**) and JPEG quality (`58%` / `72%` / `84%`), and stream smoothly at **20–24 FPS** with live FPS & Core 1 decode telemetry.
   - **Switch Display Mode (`LCD Mode`):** Select **Mode 0 (ST7789 240×240)** or **Mode 1 (ST7735 128×160)** and reboot directly into the selected screen profile.
   - Configure and save new **Wi-Fi SSID & Password** to NVS.

---

## Build & Flash

### Using PlatformIO

```bash
# Build firmware
pio run

# Upload firmware to ESP32
pio run -t upload

# Open Serial Monitor (115200 baud)
pio device monitor -b 115200
```

---

## Project Structure

```text
├── include/
│   └── radio_config.h                 # Hardware pinout, RTOS priorities & static state struct
├── src/
│   ├── main.c                         # System init, Flash sin/cos LUT & FreeRTOS task creation
│   ├── button_handler.c               # GPIO 0 IRAM ISR, press/release debounce & GPIO 33 mute
│   ├── audio_stream.c                 # HTTP ICY stream client, metadata parser & I2S DMA task
│   ├── ui_st7789.c                    # ST7789 SPI DMA scanline dirty-rectangle vinyl UI engine
│   └── wifi_web.c                     # Wi-Fi STA/AP manager, NVS storage & HTTP control server
├── AI_SYSTEM_ARCHITECTURE_PROMPT.txt  # Master architectural blueprint & porting guide for AI
└── platformio.ini                     # Build configuration (-O2 optimization, ESP32-WROOM-32)
```
