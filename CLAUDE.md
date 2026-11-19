# HomeTiles Claude entry point

@AGENTS.md
@PROJECT_CONTEXT.md

## Backlight control

- `waveshare_touch_lcd_7` (7-inch with backlight): GPIO 26, active-high, LEDC PWM 5000 Hz, 10-bit resolution
- `waveshare_touch_lcd_7b` (7B/7B-C, has backlight): GPIO 32, active-low, LEDC PWM 5000 Hz, 10-bit resolution
- `waveshare_s3_touch_lcd_7`: CH422G expander bit 2 controls AP3032 enable input (0=off, non-zero=on); off at boot in HomeTiles
