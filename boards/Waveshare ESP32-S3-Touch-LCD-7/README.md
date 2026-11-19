# Waveshare ESP32-S3-Touch-LCD-7

800x480 ST7262 RGB panel with CH422G I/O expander and GT911 capacitive touch, ESP32-S3 with 8 MB PSRAM, microSD card slot in SPI mode.

## Device profile: `waveshare_s3_touch_lcd_7`

```cpp
Device::Profile{
    "waveshare_s3_touch_lcd_7",
    "Waveshare ESP32-S3-Touch-LCD-7",
    800, 480, 5, 4, 10, 3, 150, 111, 4, 1,
    RotationStepMode::FlipOnly, 0, 2,
    Capabilities{false, false, false, false, false, false},  // No camera
    kHardwareIoProfile,
}
```

## Hardware

- **Panel**: ST7262 RGB, 800x480, 24-bit color, 50 Hz
- **Touch**: GT911 capacitive controller (I2C, address 0x5D)
- **Expander**: CH422G I2C (address 0x24)
  - Bit 2 (SD card select)
  - Bit 3 (backlight enable to AP3032 regulator)
  - Bit 4 (touch INT)
  - Bit 5 (touch RST)
- **PSRAM**: 8 MB octal PSRAM for LVGL framebuffer
- **Storage**: LittleFS (8 MB) on flash, microSD in SPI mode on the expander
- **No camera**: The ST7262 RGB panel has no CSI connector

## Pin wiring (via CH422G I2C expander at 0x24)

| CH422G bit | Function | GPIO |
|------------|----------|------|
| 2 | SD card select (SPI 0 CS2) | GPIO 26 |
| 3 | Backlight enable (AP3032 enable) | GPIO 27 |
| 4 | Touch INT | GPIO 28 |
| 5 | Touch RST | GPIO 29 |
| 6 | Touch SDA | GPIO 30 |
| 7 | Touch SCL | GPIO 31 |

## CH422G backlight control

The backlight enable line drives the AP3032 buck regulator directly. No PWM; 0 = off, any non-zero = on. The expander is latched low before enabling to prevent a flash.

## Display lifecycle

- `displayPushPixels()` / `displayPushPixelsDMA()`: push into the active RGB framebuffer
- `displayTryFullFramePreview()`: prepare the inactive framebuffer for tear-free full-frame redraw
- `displayBeginAtomicFrame()`: reserve the inactive framebuffer for a full-screen redraw
- `displayWaitDMA()`: wait for the RGB controller to finish DMA
- `displayFillScreen()`: fill the active framebuffer with a solid color
- `displaySetRotation()`: flip 0 ↔ 2 (normal / 180°)
- `displaySetUpright()`: quarter-turn the 480x800 LVGL screen into 800x480 (portrait layout)
- `displaySleep()` / `displayWake()` / `displayWakeDark()`: power states
- `displayPowerSaveOn()` / `displayPowerSaveOff()`: standby modes
- `displayWaitDisplay()`: wait for the panel to fully stabilize after a mode change

## Storage

- `initLittleFS()`: format and mount LittleFS on flash (8 MB)
- `migrateStorageFromSD()`: on first boot, copy all LittleFS files to the microSD slot
- `suspendSDCardForNetworkTransition()` / `resumeSDCardAfterNetworkTransition()`: drop SD for OTA, resume after
- `storageReady()` / `storageFS()`: access LittleFS as the primary storage backend
- `sdReady()` / `sdFS()`: access microSD as a secondary storage backend

## Integration points

### Device selection

```cpp
// src/devices/device_select.h line 17
#define DEVICE_WAVESHARE_S3_TOUCH_LCD_7
```

### Board definition

```cpp
// boards/Waveshare ESP32-S3-Touch-LCD-7/HomeTiles.ino
#include "src/devices/waveshare_s3_touch_lcd_7/device_waveshare_s3_touch_lcd_7.h"

DeviceWaveshareS3TouchLCD7::init();
```

### Storage fallback

When LittleFS is full, HomeTiles falls back to the microSD slot on the CH422G expander. The `waveshare_s3_touch_lcd_7` profile provides both `storageFS()` and `sdFS()` with a migration path.
