# Waveshare ESP32-S3-Touch-LCD-7

Exact-board profile for the 7-inch ESP32-S3 panel (`waveshare_s3_touch_lcd_7`).
Hardware facts below come from Waveshare's ESP32-S3-Touch-LCD-7 support
package: the Arduino `esp_panel_board_custom_conf.h`, the ESP-IDF
`waveshare_rgb_lcd_port.c`, the SD example and the schematic. Runtime
validation on this board is still pending.

## Board

- ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB octal PSRAM.
- 800x480 ST7262 RGB panel, 16-bit RGB565.
- GT911 capacitive touch.
- CH422G I/O expander on the shared I2C bus.
- microSD slot in SPI mode.
- CAN and RS485 headers are not used by HomeTiles.

## Display

- PCLK 12 MHz (vendor BSP: 16 MHz), falling-edge sampling (`pclk_active_neg`).
- Horizontal and vertical timing 4/8/8 (pulse/back/porch).
- HSYNC 46, VSYNC 3, DE 5, PCLK 7.
- Data bus, DATA0..DATA15:
  - B0..B4: 14, 38, 18, 17, 10
  - G0..G5: 39, 0, 45, 48, 47, 21
  - R0..R4: 1, 2, 42, 41, 40
- Direct PSRAM scanout (`bounce_buffer_size_px = 0`).

The pixel clock and scanout mode deliberately differ from the vendor's 16 MHz
bounce-buffer configuration. The stock Arduino SDK has no PSRAM XIP and
32-byte data cache lines, so the cache-fed bounce path is only safe with
PSRAM XIP and 64-byte cache lines; 16 MHz underruns during camera JPEG decode
and LVGL flushes and leaves the image shifted to the right. The driver masks
the IDF VSYNC restart because the ESP32-S3 restart link is wired to
framebuffer 0 and would defeat double buffering, so an underrun is only
repaired by an explicit restart. 12 MHz direct scanout matches the other S3
release profiles; the Waveshare LCD-4/4B run at 10 MHz if more margin is
needed.

The panel has no serial control interface; it only needs the reset pulse.
HomeTiles uses the profile's classic 5x4 grid and the normal/flipped
(0/180 degree) rotations, because the ESP32-S3 RGB peripheral cannot
quarter-turn a framebuffer.

The upright (portrait) layout is supported: LVGL draws a 480x800 screen and
the driver turns every flushed area a quarter into the 800x480 framebuffer
(`displaySetUpright`, `p4_dsi_cpu_rotate::rotate_into`). The shared
DisplayManager touch turn maps the panel point back to the upright UI for
both 0 and 180 degree settings.

The popup/setup card is 480x480 px, so the Settings pages, first-start setup
and WiFi keyboard use the compact 480-class geometry. That keeps the network
list scrollable at 480 px height and leaves the password field above the
keyboard. The tile grid and tile fonts keep their 800x480 sizes.

## CH422G expander

WR-SET address `0x24`, WR-IO address `0x38`. The driver writes `0x01` to
WR-SET to enable the outputs. Output assignments:

| Bit | Net | Use |
| --- | --- | --- |
| EXIO1 | CTP_RST | GT911 reset, active low |
| EXIO2 | DISP | Backlight regulator enable |
| EXIO3 | LCD_RST | Panel reset, active low |
| EXIO4 | SDCS | microSD chip select, active low |
| EXIO5 | USB_SEL | Kept low for the USB port |
| EXIO6 | LCD_VDD_EN | Kept high as in the vendor board definition |

The backlight is switched by the regulator enable only; there is no PWM pin,
so HomeTiles maps brightness 0 to off and every other value to on.

## Touch

GT911 on SDA 8 / SCL 9 at 400 kHz. GPIO 4 is driven low while the expander
releases CTP_RST, which selects the primary `0x5D` address; the driver probes
`0x14` as a fallback and returns the pin to input afterwards.

## microSD

SPI on SCK 12, MISO 13, MOSI 11. The chip select is EXIO4; the card is the
only device on this bus. The driver holds EXIO4 low for SD transactions, as
Waveshare's Arduino SD example does, and mounts at 4 MHz with a 1 MHz
fallback. Runtime data stays in LittleFS; the first boot migrates tile data
from the card.
