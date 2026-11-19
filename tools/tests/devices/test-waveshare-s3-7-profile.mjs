// Exact-board contract for the Waveshare ESP32-S3-Touch-LCD-7. The values
// below come from the vendor board definition, ESP-IDF LVGL port and
// schematic in the ESP32-S3-Touch-LCD-7 support package.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');

function read(relativePath) {
  return fs.readFileSync(path.join(repoRoot, relativePath), 'utf8')
    .replace(/\r\n?/g, '\n');
}

function requireMarker(source, marker, label) {
  if (!source.includes(marker)) {
    throw new Error(`${label} is missing: ${marker}`);
  }
}

function constantValue(source, name) {
  const match = source.match(new RegExp(
    `constexpr\\s+(?:u?int(?:8|16|32)_t|size_t)\\s+${name}\\s*=\\s*([^;]+);`));
  if (!match) throw new Error(`Constant was not found: ${name}`);
  return match[1].trim();
}

const header = read(
  'src/devices/waveshare_s3_touch_lcd_7/device_waveshare_s3_touch_lcd_7.h');
const driver = read(
  'src/devices/waveshare_s3_touch_lcd_7/device_waveshare_s3_touch_lcd_7.cpp');
const hardwareIo = read(
  'src/devices/waveshare_s3_touch_lcd_7/hardware_io_profile.h');

for (const marker of [
  'namespace DeviceWaveshareS3TouchLCD7',
  '"waveshare_s3_touch_lcd_7"',
  '"Waveshare ESP32-S3-Touch-LCD-7"',
  '    800,\n    480,\n    5,\n    4,\n    10,',
  'Device::RotationStepMode::FlipOnly,\n    0,\n    2,',
  'Device::Capabilities{false, false, false, false, false, false}',
]) {
  requireMarker(header, marker, 'Waveshare S3 7 device profile');
}

const expectedConstants = new Map([
  ['kExpanderAddress', '0x24'],
  ['kExpanderOutputAddress', '0x38'],
  ['kExpanderTouchReset', '1U << 1'],
  ['kExpanderBacklight', '1U << 2'],
  ['kExpanderLcdReset', '1U << 3'],
  ['kExpanderSdCs', '1U << 4'],
  ['kExpanderUsbSelect', '1U << 5'],
  ['kExpanderLcdVddEnable', '1U << 6'],
  ['kI2cSda', '8'],
  ['kI2cScl', '9'],
  ['kTouchInterruptPin', '4'],
  ['kPanelDe', '5'],
  ['kPanelVsync', '3'],
  ['kPanelHsync', '46'],
  ['kPanelPclk', '7'],
  ['kPanelB0', '14'],
  ['kPanelB1', '38'],
  ['kPanelB2', '18'],
  ['kPanelB3', '17'],
  ['kPanelB4', '10'],
  ['kPanelG0', '39'],
  ['kPanelG1', '0'],
  ['kPanelG2', '45'],
  ['kPanelG3', '48'],
  ['kPanelG4', '47'],
  ['kPanelG5', '21'],
  ['kPanelR0', '1'],
  ['kPanelR1', '2'],
  ['kPanelR2', '42'],
  ['kPanelR3', '41'],
  ['kPanelR4', '40'],
  // Deliberate deviation from the vendor BSP's 16 MHz: the stock SDK has no
  // PSRAM XIP or 64-byte cache line, so it uses direct PSRAM scanout instead
  // of the vendor's cache-fed bounce buffers. At 16 MHz the scanout underran
  // during camera JPEG decode and LVGL flushes; the driver masks the IDF
  // VSYNC restart to keep double buffering, so the image stayed shifted
  // right. 12 MHz matches the direct-scanout clock of the other S3 profiles;
  // the Waveshare LCD-4/4B use 10 MHz for more margin.
  ['kRgbPclkHz', '12000000'],
  ['kRgbBounceBufferRows', '0'],
  ['kRgbHsyncPulseWidth', '4'],
  ['kRgbHsyncBackPorch', '8'],
  ['kRgbHsyncFrontPorch', '8'],
  ['kRgbVsyncPulseWidth', '4'],
  ['kRgbVsyncBackPorch', '8'],
  ['kRgbVsyncFrontPorch', '8'],
  ['kSdSck', '12'],
  ['kSdMiso', '13'],
  ['kSdMosi', '11'],
]);
for (const [name, expected] of expectedConstants) {
  const actual = constantValue(driver, name);
  if (actual !== expected) {
    throw new Error(`${name} mismatch: expected ${expected}, got ${actual}`);
  }
}

for (const marker of [
  'constexpr uint32_t kExpectedFlashBytes = 16U * 1024U * 1024U;',
  'constexpr uint32_t kExpectedPsramBytes = 8U * 1024U * 1024U;',
  'flash_bytes != kExpectedFlashBytes',
  'psram_bytes != kExpectedPsramBytes',
  'config.timings.flags.pclk_active_neg = 1;',
  'config.num_fbs = 2;',
  'config.flags.double_fb = true;',
  'kPanelB0, kPanelB1, kPanelB2, kPanelB3, kPanelB4,\n'
    + '        kPanelG0, kPanelG1, kPanelG2, kPanelG3, kPanelG4, kPanelG5,\n'
    + '        kPanelR0, kPanelR1, kPanelR2, kPanelR3, kPanelR4',
  'if (setExpanderBit(kExpanderBacklight, value != 0)) {',
  'pinMode(kTouchInterruptPin, OUTPUT);\n'
    + '  digitalWrite(kTouchInterruptPin, LOW);',
  'pinMode(kTouchInterruptPin, INPUT);',
  'setExpanderBit(kExpanderSdCs, false);',
  'SD.begin(static_cast<uint8_t>(-1), g_sd_spi, kSdFrequency,',
  'copyDirectory(SD, LittleFS, "/_tile_grids");',
  'x = static_cast<int16_t>(kMaxX - static_cast<int16_t>(raw_x));\n'
    + '    y = static_cast<int16_t>(kMaxY - static_cast<int16_t>(raw_y));',
  'g_rotation = rotation & 0x02;',
  'const uint32_t now = millis();\n'
    + '    if (g_touch_init_attempted &&\n'
    + '        (now - g_touch_retry_tick_ms) < kTouchRetryMs) {',
  'g_backlight_state_unknown = true;',
  'const bool blackout = g_backlight_ready &&\n'
    + '                        (g_backlight_state_unknown ||\n'
    + '                         g_applied_brightness != 0);',
  'bool copyDirectory(fs::FS& src_fs, fs::FS& dst_fs, const char* dir_path) {',
  'bool migration_ok = have_sd;',
  'if (migration_ok) {',
]) {
  requireMarker(driver, marker, 'Waveshare S3 7 hardware contract');
}

for (const forbidden of [
  'st7701',
  'ledcAttach',
  'kBacklightPin',
  'QuarterTurns',
  'DEVICE_WAVESHARE_TOUCH_LCD_7',
  'Guition',
  'guition',
]) {
  if (driver.includes(forbidden)) {
    throw new Error(`Waveshare S3 7 driver contains forbidden marker: ${forbidden}`);
  }
}

requireMarker(hardwareIo, 'kHardwareIoProfile{}', 'Hardware I/O profile');
if (/\{\s*\d+\s*,/.test(hardwareIo)) {
  throw new Error('Reserved Waveshare S3 7 pins must not be exposed as I/O');
}

console.log('Waveshare ESP32-S3-Touch-LCD-7 profile contract: PASS');
