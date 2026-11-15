# ESP32-WROOM-32 Retro Vinyl Web Radio

Internet radio player & retro vinyl visualizer for the **ESP32-WROOM-32** built with pure **ESP-IDF (v5.3+) & FreeRTOS**. Streams live web radio over Wi-Fi to a **MAX98357A I2S** amplifier, renders a 30 FPS rotating vinyl record UI on a **1.54" ST7789 (240×240)** IPS display using scanline DMA dirty-rectangles (no PSRAM required), and includes an instant hardware **Play / Pause** button plus a built-in **Web Control & Wi-Fi Setup** page.

---

## Wiring

### ST7789 1.54" IPS TFT Display (240×240 SPI)

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

- **Retro "Vinyl Player — Now Playing" UI (240×240 RGB565):**
  - Warm analog cream canvas (`#F4F1EA`), rotating pitch-black vinyl record with concentric grooves, anisotropic light sheen, terracotta center label (`#C84B31`), and brass tonearm that smoothly lifts on Pause and drops onto the record on Play.
  - **Live Top-Left Wi-Fi IP Display:** Automatically displays the assigned STA IP address (`e.g. 192.168.100.186`) in the top-left corner as soon as Wi-Fi connects.
  - **Scanline Dirty-Rectangle DMA Engine:** Uses a single shared `19.2 KB` DMA buffer instead of a `115.2 KB` full-frame buffer, saving **>95 KB of internal SRAM** while rendering tear-free at ~30 FPS.
  - **Fixed-Point Q10 Trigonometry (`>> 10`):** Precomputed `sin_lut[360]` and `cos_lut[360]` stored in Flash (`rodata`) — zero floating-point (`float`/`sqrt`) operations in the render loop.
- **Pop-Free Instant Hardware Mute & Dual-Edge Debounce:**
  - Pressing `GPIO 0` pulls `GPIO 33 (SD)` low in `< 1 µs` to silence the MAX98357A before flushing the I2S DMA pipeline, eliminating audio pop/click artifacts.
  - Two-stage ISR + Task press-and-release verification filters out mechanical switch bounce on both press and release edges.
- **Zero-Heap-Fragmentation Runtime:**
  - All system state is consolidated in a single static `.bss` structure (`g_radio`) synchronized via `<stdatomic.h>` flags, FreeRTOS `EventGroup`, `StreamBuffer`, and `TaskNotify`.

### FreeRTOS Multi-Core Task Allocation

| Task Name | Core | Priority | Responsibility |
| :--- | :---: | :---: | :--- |
| `audio_i2s_task` | Core 1 | **16** | Reads pre-buffered audio from `StreamBuffer`, applies integer volume scaling, writes to I2S DMA |
| `button_ctrl_task` | Core 1 | **15** | Woken by `GPIO 0` `IRAM_ATTR` ISR; verifies press/release debounce & toggles hardware mute + Play/Pause |
| `wifi_and_web_task` | Core 0 | **7** | Manages Wi-Fi STA/AP (`WIFI_PS_NONE`), NVS credential recovery, and embedded `esp_http_server` |
| `http_stream_task` | Core 0 | **5** | Streams HTTP audio, parses in-stream `icy-metaint` (`StreamTitle`), feeds `16 KB` `StreamBuffer` |
| `ui_render_task` | Core 1 | **4** | Renders dirty rectangles via SPI2 DMA (~30 FPS when playing, 5 FPS power-save when paused) |

---

## Web UI & Wi-Fi Configuration

1. **Automatic STA Connection:** On boot, the ESP32 loads saved Wi-Fi credentials from NVS and prints/displays its local IP at the **top-left corner of the TFT screen** (e.g., `192.168.100.186`).
2. **Fallback Access Point:** If no saved Wi-Fi is found, connect to the hotspot **`ESP32-WebRadio-AP`** (password: `12345678`) and open `http://192.168.4.1/`.
3. **Web Dashboard (`http://<ESP32_IP>/`):**
   - View live station name, song title (`ICY StreamTitle`), and playback state.
   - Toggle **Play / Pause**, switch radio stations, and adjust **Volume (0–100%)**.
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
