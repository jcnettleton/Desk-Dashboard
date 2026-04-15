# Waveshare 5.79" E-Paper ESP32-S3 — Board Notes

Reference document for continuing development on this board.

---

## Hardware Overview

| Component | Detail |
|---|---|
| **MCU** | ESP32-S3-WROOM-1-N8R8 (QFN56, revision v0.2) |
| **Flash** | 8 MB |
| **PSRAM** | 8 MB (OPI mode — **must** be configured correctly or large buffers crash) |
| **Display** | 5.79" B/W E-Ink, 792×272 pixels, 4 grey scale capable |
| **Display Controller** | Dual SSD1683 (each IC drives half the screen) |
| **Display Interface** | SPI |
| **USB-to-Serial** | CH340 (VID: `1A86`, PID: `7523`) |
| **Serial Port** | `/dev/ttyUSB0` @ 115200 baud |
| **MAC Address** | `44:1b:f6:95:53:98` |
| **Battery** | SH1.0-2P connector, LTC4054 charger, 3.7V LiPo |
| **Storage** | TF / MicroSD card slot |

---

## E-Paper SPI Pin Mapping (ESP32-S3 GPIOs)

| Signal | GPIO |
|---|---|
| **CLK / SCK** | 12 |
| **MOSI (Data)** | 11 |
| **CS (Chip Select)** | 45 |
| **DC (Data/Command)** | 46 |
| **RST (Reset)** | 47 |
| **BUSY** | 48 |
| **EPD_POWER** | 7 |

### ⚠️ Power Control (Critical)

GPIO 7 controls the display power rail. You **must**:

1. Set GPIO 7 as OUTPUT
2. Pull it HIGH
3. Wait ~500 ms

...before initializing or writing to the display. Without this, the screen will not respond (you'll see "Busy Timeout!" on serial).

---

## GxEPD2 Display Driver

The correct GxEPD2 class for this panel is:

```
GxEPD2_579_GDEY0579T93  — GDEY0579T93, 792×272, SSD1683, B/W
```

Constructor:
```cpp
GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT>
    display(GxEPD2_579_GDEY0579T93(/*CS=*/ 45, /*DC=*/ 46, /*RST=*/ 47, /*BUSY=*/ 48));
```

The library handles the dual-IC SSD1683 internally for this driver.

---

## PlatformIO Configuration

Current `platformio.ini` (needs updating — see "Still TODO" below):

```ini
[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
upload_port = /dev/ttyUSB0
monitor_port = /dev/ttyUSB0
monitor_speed = 115200
lib_deps =
    zinggjm/GxEPD2@^1.6.8
    adafruit/Adafruit GFX Library@^1.12.6
```

### Recommended additions for this board:

```ini
board_build.arduino.memory_type = qio_opi   ; Enable OPI PSRAM
board_build.partitions = huge_app.csv        ; 3MB app / 1MB SPIFFS
board_upload.flash_size = 8MB
build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
```

---

## Current `main.cpp` Status

The existing code **compiles and uploads** but uses **wrong settings**:

- ❌ Pin mapping: CS=5, DC=17, RST=16, BUSY=4 (generic ESP32 defaults)
- ❌ Display driver: `GxEPD2_290_BS` (2.9" panel, wrong size)
- ❌ No EPD power pin control (GPIO 7)
- ❌ No custom SPI pin assignment (SCK=12, MOSI=11)

These are the reasons the serial output showed "Busy Timeout!" followed by "Display updated!" — the display never actually received data.

---

## Still TODO

1. **Update `platformio.ini`** — add PSRAM/OPI config, partition scheme, flash size
2. **Rewrite `main.cpp`** with:
   - Correct pin defines (CS=45, DC=46, RST=47, BUSY=48, SCK=12, MOSI=11, PWR=7)
   - GPIO 7 power-on sequence before `display.init()`
   - Custom SPI bus: `SPI.begin(12, -1, 11, 45)` (SCK, MISO unused, MOSI, CS)
   - Correct display driver: `GxEPD2_579_GDEY0579T93`
3. **Build & flash** — verify "Hello, World!" appears on the 5.79" screen
4. **Explore other peripherals** — dial switch, buttons, SD card, battery

---

## Other On-Board Interfaces

| Feature | Details |
|---|---|
| **GPIO Header** | 2×10 DuPont, 12 usable GPIOs (3, 8, 9, 14, etc.), 4× 3.3V, 4× GND |
| **Controls** | 3-way dial switch (up/down/push), Menu button, Back button |
| **System Buttons** | BOOT, RESET |
| **SD Card** | TF/MicroSD slot (for images, fonts, data) |
| **Battery** | SH1.0-2P, LTC4054 charger, 3.7V LiPo |

---

## Environment Notes

- **OS**: Manjaro Linux (Arch-based)
- **VS Code**: Running as Flatpak (`com.visualstudio.code`)
- **Flatpak fix**: Device access required `flatpak override --user com.visualstudio.code --device=all`
- **PlatformIO CLI**: `~/.platformio/penv/bin/pio` (added to PATH in `~/.bashrc`)
- **User group**: Added to `uucp` for serial port access (`sudo usermod -aG uucp john`)
- **Platform version**: espressif32@6.13.0, framework-arduinoespressif32@3.20017.241212
