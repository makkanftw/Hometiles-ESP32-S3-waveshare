#include "src/devices/waveshare_s3_touch_lcd_7/device_waveshare_s3_touch_lcd_7.h"
#include "src/devices/device_select.h"

#if defined(DEVICE_WAVESHARE_S3_TOUCH_LCD_7)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_phy_init.h>
#include <esp_private/periph_ctrl.h>
#include <hal/lcd_ll.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "src/devices/common/p4_dsi_cpu_rotate.h"

namespace {

// CH422G I/O expander. WR-SET enables the outputs; WR-IO holds their levels.
// The board wires backlight, panel/touch reset and the microSD chip select to
// it, so a single I2C bus also carries the GT911.
constexpr uint8_t kExpanderAddress = 0x24;
constexpr uint8_t kExpanderOutputAddress = 0x38;
constexpr uint8_t kExpanderOutputEnable = 0x01;
constexpr uint8_t kExpanderTouchReset = 1U << 1;   // EXIO1, active low
constexpr uint8_t kExpanderBacklight = 1U << 2;    // EXIO2, active high enable
constexpr uint8_t kExpanderLcdReset = 1U << 3;     // EXIO3, active low
constexpr uint8_t kExpanderSdCs = 1U << 4;         // EXIO4, active low
constexpr uint8_t kExpanderUsbSelect = 1U << 5;    // EXIO5, low keeps USB
constexpr uint8_t kExpanderLcdVddEnable = 1U << 6; // EXIO6
// The vendor's Arduino board definition leaves EXIO6 high and selects the SD
// card only for a mount. Backlight and USB select stay low at boot.
constexpr uint8_t kExpanderInitialOutputs =
    kExpanderTouchReset | kExpanderLcdReset | kExpanderSdCs |
    kExpanderLcdVddEnable;
static_assert((kExpanderInitialOutputs & kExpanderBacklight) == 0,
              "Backlight starts off");
static_assert((kExpanderInitialOutputs & kExpanderUsbSelect) == 0,
              "USB select stays low");

constexpr int8_t kI2cSda = 8;
constexpr int8_t kI2cScl = 9;
constexpr uint32_t kI2cFrequency = 400000;

// GT911 capacitive touch; its reset line is driven by the expander and GPIO4
// selects the controller's 0x5D address during reset.
constexpr int8_t kTouchInterruptPin = 4;
constexpr uint8_t kTouchAddressPrimary = 0x5D;
constexpr uint8_t kTouchAddressAlternate = 0x14;
constexpr uint16_t kTouchProductIdRegister = 0x8140;
constexpr uint16_t kTouchStatusRegister = 0x814E;
constexpr uint16_t kTouchPointRegister = 0x814F;
constexpr uint8_t kTouchErrorReleaseThreshold = 16;
constexpr uint32_t kTouchRetryMs = 15000;

// ST7262 RGB panel from the Waveshare ESP32-S3-Touch-LCD-7 schematic. The
// 16-bit bus maps DATA0..DATA4 to B0..B4, DATA5..DATA10 to G0..G5 and
// DATA11..DATA15 to R0..R4.
constexpr int8_t kPanelDe = 5;
constexpr int8_t kPanelVsync = 3;
constexpr int8_t kPanelHsync = 46;
constexpr int8_t kPanelPclk = 7;
constexpr int8_t kPanelB0 = 14;
constexpr int8_t kPanelB1 = 38;
constexpr int8_t kPanelB2 = 18;
constexpr int8_t kPanelB3 = 17;
constexpr int8_t kPanelB4 = 10;
constexpr int8_t kPanelG0 = 39;
constexpr int8_t kPanelG1 = 0;
constexpr int8_t kPanelG2 = 45;
constexpr int8_t kPanelG3 = 48;
constexpr int8_t kPanelG4 = 47;
constexpr int8_t kPanelG5 = 21;
constexpr int8_t kPanelR0 = 1;
constexpr int8_t kPanelR1 = 2;
constexpr int8_t kPanelR2 = 42;
constexpr int8_t kPanelR3 = 41;
constexpr int8_t kPanelR4 = 40;

// Waveshare's ESP-IDF and Arduino board definitions use 16 MHz with these
// porches and a falling-edge pixel clock. On the stock Arduino SDK the RGB
// GDMA must read the PSRAM framebuffer directly: the cache-fed bounce path
// is only safe with PSRAM XIP and 64-byte cache lines, neither of which this
// SDK enables. At the vendor's 16 MHz the scanout fell behind camera JPEG
// decode and LVGL flushes and the image stayed shifted to the right,
// because this driver masks the IDF VSYNC restart to keep double buffering
// (the S3 restart link is wired to framebuffer 0). 12 MHz is the
// direct-scanout clock of the other S3 profiles in this project; the
// Waveshare LCD-4/4B use 10 MHz. If camera streaming still shifts the
// image, drop to 10 MHz.
constexpr uint32_t kRgbPclkHz = 12000000;
constexpr uint32_t kRgbHsyncPulseWidth = 4;
constexpr uint32_t kRgbHsyncBackPorch = 8;
constexpr uint32_t kRgbHsyncFrontPorch = 8;
constexpr uint32_t kRgbVsyncPulseWidth = 4;
constexpr uint32_t kRgbVsyncBackPorch = 8;
constexpr uint32_t kRgbVsyncFrontPorch = 8;
// Direct PSRAM scanout on this SDK; see the clock rationale above.
constexpr size_t kRgbBounceBufferRows = 0;
constexpr size_t kRgbBounceBufferPixels = 800 * kRgbBounceBufferRows;
constexpr uint32_t kRgbHorizontalTotal =
    800 + kRgbHsyncPulseWidth + kRgbHsyncBackPorch + kRgbHsyncFrontPorch;
constexpr uint32_t kRgbVerticalTotal =
    480 + kRgbVsyncPulseWidth + kRgbVsyncBackPorch + kRgbVsyncFrontPorch;
constexpr uint32_t kRgbFramePeriodMs =
    ((kRgbHorizontalTotal * kRgbVerticalTotal * 1000U) + kRgbPclkHz - 1U) /
    kRgbPclkHz;

// microSD in SPI mode; the card select is EXIO4 on the expander.
constexpr int8_t kSdSck = 12;
constexpr int8_t kSdMiso = 13;
constexpr int8_t kSdMosi = 11;
constexpr uint32_t kSdFrequency = 4000000;
constexpr uint32_t kSdFallbackFrequency = 1000000;
constexpr uint32_t kSdRetryMs = 1500;

constexpr uint32_t kExpectedFlashBytes = 16U * 1024U * 1024U;
constexpr uint32_t kExpectedPsramBytes = 8U * 1024U * 1024U;

#if defined(CONFIG_COMPILER_OPTIMIZATION_PERF) && \
    CONFIG_COMPILER_OPTIMIZATION_PERF
constexpr const char* kCompilerOptimization = "O2";
#else
constexpr const char* kCompilerOptimization = "size";
#endif
#if defined(CONFIG_LCD_RGB_RESTART_IN_VSYNC) && \
    CONFIG_LCD_RGB_RESTART_IN_VSYNC
constexpr bool kRestartInVsync = true;
#else
constexpr bool kRestartInVsync = false;
#endif

bool initExpander();
bool setExpanderBit(uint8_t mask, bool on);
bool resetLcdPanel();

class WaveshareAtomicRgbDisplay final : public Arduino_RGB_Display {
 public:
  WaveshareAtomicRgbDisplay(uint8_t rotation)
      : Arduino_RGB_Display(800, 480, nullptr, rotation, true, nullptr,
                            GFX_NOT_DEFINED, nullptr, 0) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    (void)speed;
    if (!resetLcdPanel()) {
      return false;
    }

    esp_lcd_rgb_panel_config_t config{};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = kRgbPclkHz;
    config.timings.h_res = 800;
    config.timings.v_res = 480;
    config.timings.hsync_pulse_width = kRgbHsyncPulseWidth;
    config.timings.hsync_back_porch = kRgbHsyncBackPorch;
    config.timings.hsync_front_porch = kRgbHsyncFrontPorch;
    config.timings.vsync_pulse_width = kRgbVsyncPulseWidth;
    config.timings.vsync_back_porch = kRgbVsyncBackPorch;
    config.timings.vsync_front_porch = kRgbVsyncFrontPorch;
    config.timings.flags.hsync_idle_low = 0;
    config.timings.flags.vsync_idle_low = 0;
    config.timings.flags.de_idle_high = 0;
    config.timings.flags.pclk_active_neg = 1;
    config.timings.flags.pclk_idle_high = 0;
    config.data_width = 16;
    config.bits_per_pixel = 16;
    config.num_fbs = 2;
    config.bounce_buffer_size_px = kRgbBounceBufferPixels;
    config.sram_trans_align = 8;
    config.psram_trans_align = 64;
    config.hsync_gpio_num = kPanelHsync;
    config.vsync_gpio_num = kPanelVsync;
    config.de_gpio_num = kPanelDe;
    config.pclk_gpio_num = kPanelPclk;
    config.disp_gpio_num = GPIO_NUM_NC;
    const int data_pins[16] = {
        kPanelB0, kPanelB1, kPanelB2, kPanelB3, kPanelB4,
        kPanelG0, kPanelG1, kPanelG2, kPanelG3, kPanelG4, kPanelG5,
        kPanelR0, kPanelR1, kPanelR2, kPanelR3, kPanelR4};
    std::copy(std::begin(data_pins), std::end(data_pins),
              config.data_gpio_nums);
    config.flags.disp_active_low = false;
    config.flags.refresh_on_demand = false;
    config.flags.fb_in_psram = true;
    config.flags.double_fb = true;
    config.flags.no_fb = false;
    config.flags.bb_invalidate_cache = false;

    esp_err_t err = esp_lcd_new_rgb_panel(&config, &panel_handle_);
    if (err == ESP_OK) {
      esp_lcd_rgb_panel_event_callbacks_t callbacks{};
      callbacks.on_vsync = onVsync;
      callbacks.on_frame_buf_complete = onFrameComplete;
      err = esp_lcd_rgb_panel_register_event_callbacks(
          panel_handle_, &callbacks, this);
    }
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel_handle_);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel_handle_);
    if (err == ESP_OK) {
      // panel_init() starts the stream and enables VSYNC. Mask immediately,
      // before any further setup work can let Arduino-ESP32's automatic
      // CONFIG_LCD_RGB_RESTART_IN_VSYNC path fire once at a random phase.
      maskVsyncInterrupt();
    }
    if (err == ESP_OK) {
      err = esp_lcd_rgb_panel_get_frame_buffer(
          panel_handle_, 2, reinterpret_cast<void**>(&framebuffers_[0]),
          reinterpret_cast<void**>(&framebuffers_[1]));
    }
    if (err != ESP_OK || !framebuffers_[0] || !framebuffers_[1]) {
      Serial.printf(
          "[Display/S3] Double framebuffer init failed: %s (0x%X)\n",
          esp_err_to_name(err), static_cast<unsigned>(err));
      return false;
    }

    active_index_ = 0;
    pending_index_ = 0;
    atomic_pending_ = false;
    canonical_fb0_valid_ = true;
    _framebuffer = framebuffers_[0];
    return true;
  }

  // The upright layout on this landscape panel: LVGL draws a 480x800 screen;
  // each flushed area is turned a quarter into the 800x480 framebuffer. The
  // place matches the 7B/JC1060 drivers, so DisplayManager's shared touch
  // turn maps the panel point back to the upright UI.
  void drawTurnedArea(int32_t x, int32_t y, int32_t w, int32_t h,
                      const uint16_t* data, uint8_t rotation) {
    if (!_framebuffer || !data || w <= 0 || h <= 0) return;
    const uint8_t turn = rotation ^ 0x02;
    const int32_t panel_w = static_cast<int32_t>(_fb_width);
    const int32_t panel_h = static_cast<int32_t>(_fb_height);
    const int32_t dst_x = (turn & 0x02) ? y : panel_w - y - h;
    const int32_t dst_y = (turn & 0x02) ? panel_h - x - w : x;
    if (dst_x < 0 || dst_y < 0 || dst_x + h > panel_w ||
        dst_y + w > panel_h) {
      return;
    }
    p4_dsi_cpu_rotate::rotate_into(
        _framebuffer, _fb_width, dst_x, dst_y, w, h, data,
        (turn & 0x02) != 0);
    // rotate_into() writes cached PSRAM; push every destination row before the
    // next RGB frame can read it.
    for (int32_t row = 0; row < w; ++row) {
      Cache_WriteBack_Addr(
          reinterpret_cast<uint32_t>(
              _framebuffer + static_cast<size_t>(dst_y + row) * _fb_width +
              dst_x),
          static_cast<uint32_t>(h) * sizeof(uint16_t));
    }
  }

  bool beginAtomicFrame(const char* reason) {
    if (!panel_handle_ || !framebuffers_[0] || !framebuffers_[1]) {
      return false;
    }
    if (storage_transition_) return false;
    if (atomic_pending_) return true;

    pending_index_ = active_index_ ^ 1U;
    if (pending_index_ == 0) canonical_fb0_valid_ = false;
    _framebuffer = framebuffers_[pending_index_];
    atomic_pending_ = true;
    atomic_started_ms_ = millis();
    atomic_reason_ = reason ? reason : "unknown";
    return true;
  }

  bool commitAtomicFrame() {
    if (!atomic_pending_ || !panel_handle_) return false;

    flush(true);
    if (pending_index_ == 0) canonical_fb0_valid_ = true;
    const uint32_t eof_start = frame_complete_count_;
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        panel_handle_, 0, 0, _fb_width, _fb_height,
        framebuffers_[pending_index_]);
    const bool presented =
        err == ESP_OK && waitForFrameCompletions(eof_start, 3,
                                                 kRgbFramePeriodMs * 5U + 20U);
    if (err == ESP_OK) {
      // Three EOFs cover the current frame, the one additional old frame that
      // IDF says DMA prefetch may emit, and the completed new frame.
      active_index_ = pending_index_;
      canonical_fb0_valid_ = active_index_ == 0;
    }
    _framebuffer = framebuffers_[active_index_];
    atomic_pending_ = false;
    atomic_reason_ = "none";
    atomic_started_ms_ = 0;
    return err == ESP_OK && presented;
  }

  void service() {
    // A software JPEG decode on this S3 can legitimately take several
    // seconds before LVGL reaches the first full-screen flush. Keep a generous
    // failsafe for truly abandoned transitions without cancelling valid work.
    if (!atomic_pending_ || atomic_started_ms_ == 0 ||
        millis() - atomic_started_ms_ < 15000U) {
      return;
    }
    _framebuffer = framebuffers_[active_index_];
    atomic_pending_ = false;
    atomic_started_ms_ = 0;
    Serial.printf(
        "[Display/S3] Atomic redraw timeout, keeping framebuffer %u\n",
        static_cast<unsigned>(active_index_));
    atomic_reason_ = "none";
  }

  bool canonicalizeForStorage() {
    if (!panel_handle_) return false;
    storage_transition_ = true;
    if (atomic_pending_) {
      commitAtomicFrame();
    }
    if (active_index_ == 0) {
      _framebuffer = framebuffers_[0];
      canonical_fb0_valid_ = true;
      return true;
    }

    memcpy(framebuffers_[0], framebuffers_[active_index_], _framebuffer_size);
    // The CPU copy lands in cached PSRAM. Write the complete canonical frame
    // back before RGB GDMA is allowed to scan framebuffer 0.
    Cache_WriteBack_Addr(
        reinterpret_cast<uint32_t>(framebuffers_[0]), _framebuffer_size);
    canonical_fb0_valid_ = true;
    const uint32_t eof_start = frame_complete_count_;
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        panel_handle_, 0, 0, _fb_width, _fb_height, framebuffers_[0]);
    if (err == ESP_OK) {
      waitForFrameCompletions(eof_start, 3, kRgbFramePeriodMs * 5U + 20U);
    }
    if (err == ESP_OK) {
      active_index_ = 0;
    }
    _framebuffer = framebuffers_[active_index_];
    // Even if draw_bitmap or the diagnostic EOF wait fails, framebuffer 0 is
    // a complete cache-flushed copy. The storage-end one-shot restart is
    // hardwired by IDF to this canonical buffer and can still recover it.
    return canonical_fb0_valid_;
  }

  esp_err_t restartAfterStorage(uint32_t& wait_ms) {
    wait_ms = 0;
    if (!panel_handle_ || !canonical_fb0_valid_) {
      storage_transition_ = false;
      return ESP_ERR_INVALID_STATE;
    }

    restart_vsync_seen_ = false;
    restart_one_shot_armed_ = true;
    const uint32_t started_ms = millis();
    esp_err_t err = esp_lcd_rgb_panel_restart(panel_handle_);
    if (err == ESP_OK) {
      enableVsyncInterruptOneShot();
      while (!restart_vsync_seen_ &&
             millis() - started_ms < kRgbFramePeriodMs * 3U + 20U) {
        delay(1);
      }
      if (!restart_vsync_seen_) {
        err = ESP_ERR_TIMEOUT;
      } else {
        const uint32_t eof_start = restart_eof_baseline_;
        if (!waitForFrameCompletions(eof_start, 2,
                                     kRgbFramePeriodMs * 4U + 20U)) {
          err = ESP_ERR_TIMEOUT;
        }
      }
    }
    maskVsyncInterrupt();
    restart_one_shot_armed_ = false;
    if (restart_vsync_seen_) {
      // The current VSYNC ISR performs the hardwired S3 restart to fb0 after
      // invoking our callback, even when the subsequent EOF confirmation
      // times out.
      active_index_ = 0;
      _framebuffer = framebuffers_[0];
    }
    wait_ms = millis() - started_ms;
    storage_transition_ = false;
    return err;
  }

 private:
  static bool IRAM_ATTR onFrameComplete(
      esp_lcd_panel_handle_t panel,
      const esp_lcd_rgb_panel_event_data_t* event_data, void* user_ctx) {
    (void)panel;
    (void)event_data;
    auto* self = static_cast<WaveshareAtomicRgbDisplay*>(user_ctx);
    if (self) ++self->frame_complete_count_;
    return false;
  }

  static bool IRAM_ATTR onVsync(
      esp_lcd_panel_handle_t panel,
      const esp_lcd_rgb_panel_event_data_t* event_data, void* user_ctx) {
    (void)panel;
    (void)event_data;
    auto* self = static_cast<WaveshareAtomicRgbDisplay*>(user_ctx);
    if (!self || !self->restart_one_shot_armed_) return false;

    // The IDF ISR calls this callback before its restart routine. Masking the
    // hardware bit here prevents future VSYNC interrupts; the current ISR
    // continues and performs exactly one restart through canonical fb0.
    PERIPH_RCC_ATOMIC() {
      lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, false);
    }
    self->restart_one_shot_armed_ = false;
    self->restart_eof_baseline_ = self->frame_complete_count_;
    self->restart_vsync_seen_ = true;
    return false;
  }

  static void maskVsyncInterrupt() {
    PERIPH_RCC_ATOMIC() {
      lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, false);
      lcd_ll_clear_interrupt_status(&LCD_CAM, LCD_LL_EVENT_RGB);
    }
  }

  static void enableVsyncInterruptOneShot() {
    PERIPH_RCC_ATOMIC() {
      // A disabled VSYNC source remains latched. Clear it before enabling or
      // the ISR can restart DMA immediately in the middle of an active frame.
      lcd_ll_clear_interrupt_status(&LCD_CAM, LCD_LL_EVENT_RGB);
      lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, true);
    }
  }

  bool waitForFrameCompletions(uint32_t start, uint32_t count,
                               uint32_t timeout_ms) const {
    const uint32_t started_ms = millis();
    while (static_cast<uint32_t>(frame_complete_count_ - start) < count &&
           millis() - started_ms < timeout_ms) {
      delay(1);
    }
    return static_cast<uint32_t>(frame_complete_count_ - start) >= count;
  }

  esp_lcd_panel_handle_t panel_handle_ = nullptr;
  uint16_t* framebuffers_[2] = {nullptr, nullptr};
  uint8_t active_index_ = 0;
  uint8_t pending_index_ = 0;
  bool atomic_pending_ = false;
  bool storage_transition_ = false;
  bool canonical_fb0_valid_ = true;
  uint32_t atomic_started_ms_ = 0;
  const char* atomic_reason_ = "none";
  volatile uint32_t frame_complete_count_ = 0;
  volatile bool restart_one_shot_armed_ = false;
  volatile bool restart_vsync_seen_ = false;
  volatile uint32_t restart_eof_baseline_ = 0;
};

WaveshareAtomicRgbDisplay* g_gfx = nullptr;
SPIClass g_sd_spi(FSPI);

bool g_expander_ready = false;
uint8_t g_expander_outputs = 0;
bool g_display_ready = false;
bool g_backlight_ready = false;
bool g_touch_ready = false;
bool g_touch_init_attempted = false;
uint32_t g_touch_retry_tick_ms = 0;
bool g_littlefs_ready = false;
bool g_sd_init_attempted = false;
bool g_sd_available = false;
uint32_t g_sd_retry_tick_ms = 0;
uint8_t g_brightness = 0;
uint8_t g_applied_brightness = 0;
bool g_backlight_state_unknown = false;
bool g_turned = false;
uint8_t g_rotation = DeviceWaveshareS3TouchLCD7::kProfile.rotation_default;
uint8_t g_touch_address = 0;
uint16_t g_storage_write_depth = 0;
bool g_storage_blackout_active = false;
bool g_storage_restart_required = false;
uint8_t g_storage_restore_brightness = 0;
bool g_touch_active = false;
int16_t g_touch_last_x = 0;
int16_t g_touch_last_y = 0;
uint8_t g_touch_status_error_streak = 0;
uint8_t g_touch_point_error_streak = 0;

void ensureStorageLayout() {
  if (!g_littlefs_ready) return;
  LittleFS.mkdir("/_tile_grids");
  LittleFS.mkdir("/_tile_links");
}

bool writeExpanderOutput(uint8_t value) {
  Wire.beginTransmission(kExpanderOutputAddress);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool initExpander() {
  if (g_expander_ready) return true;

  Wire.begin(kI2cSda, kI2cScl, kI2cFrequency);
  // Latch every output low before enabling the drivers: the CH422G powers up
  // with its output register high, which would otherwise flash the backlight
  // before the panel has been initialised.
  if (!writeExpanderOutput(0x00)) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] CH422G output init failed");
    return false;
  }
  Wire.beginTransmission(kExpanderAddress);
  Wire.write(kExpanderOutputEnable);
  if (Wire.endTransmission() != 0) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] CH422G not found at 0x24");
    return false;
  }
  if (!writeExpanderOutput(kExpanderInitialOutputs)) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] CH422G output init failed");
    return false;
  }
  g_expander_outputs = kExpanderInitialOutputs;
  g_expander_ready = true;
  return true;
}

bool setExpanderBit(uint8_t mask, bool on) {
  if (!g_expander_ready && !initExpander()) return false;
  const uint8_t next =
      on ? static_cast<uint8_t>(g_expander_outputs | mask)
         : static_cast<uint8_t>(g_expander_outputs & ~mask);
  if (next == g_expander_outputs) return true;
  if (!writeExpanderOutput(next)) return false;
  g_expander_outputs = next;
  return true;
}

bool resetLcdPanel() {
  if (!g_expander_ready && !initExpander()) return false;
  if (!setExpanderBit(kExpanderLcdReset, false)) return false;
  delay(10);
  if (!setExpanderBit(kExpanderLcdReset, true)) return false;
  delay(100);
  return true;
}

void applyBrightness(uint8_t value, bool remember = true) {
  if (remember) g_brightness = value;
  if (!g_backlight_ready) {
    if (!initExpander()) return;
    g_backlight_ready = true;
  }
  // The AP3032 backlight regulator has an enable input only; there is no PWM
  // pin on this board. Zero is off, every other level switches it on.
  if (setExpanderBit(kExpanderBacklight, value != 0)) {
    g_applied_brightness = value;
    g_backlight_state_unknown = false;
  } else {
    // Do not claim the panel is dark when the expander write failed; the
    // storage guard must still black out before a flash write.
    g_backlight_state_unknown = true;
  }
}

bool writeTouchRegister(uint16_t reg, uint8_t value) {
  if (!g_touch_address) return false;
  Wire.beginTransmission(g_touch_address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readTouchRegisters(uint16_t reg, uint8_t* data, size_t len) {
  if (!g_touch_address || !data || len == 0 || len > 32) return false;
  Wire.beginTransmission(g_touch_address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  const size_t received =
      Wire.requestFrom(static_cast<int>(g_touch_address), static_cast<int>(len));
  if (received != len) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    data[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool probeTouchAddress(uint8_t address) {
  Wire.beginTransmission(address);
  if (Wire.endTransmission() != 0) return false;
  g_touch_address = address;
  uint8_t product_id[4] = {};
  if (!readTouchRegisters(kTouchProductIdRegister, product_id,
                          sizeof(product_id))) {
    g_touch_address = 0;
    return false;
  }
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] GT911 at 0x%02X, id=%c%c%c%c\n",
      address, product_id[0], product_id[1], product_id[2], product_id[3]);
  return true;
}

bool initTouch() {
  if (g_touch_ready) return true;
  // The reset sequence blocks for about 310 ms. Latch the attempt so a missing
  // controller cannot run it again on every 8 ms input poll.
  g_touch_init_attempted = true;
  g_touch_retry_tick_ms = millis();
  if (!g_expander_ready && !initExpander()) return false;
  Wire.begin(kI2cSda, kI2cScl, kI2cFrequency);

  // Drive INT low while releasing reset: the GT911 then chooses 0x5D. The
  // vendor board definition returns the pin to a plain input afterwards.
  pinMode(kTouchInterruptPin, OUTPUT);
  digitalWrite(kTouchInterruptPin, LOW);
  delay(10);
  setExpanderBit(kExpanderTouchReset, false);
  delay(100);
  setExpanderBit(kExpanderTouchReset, true);
  delay(200);
  pinMode(kTouchInterruptPin, INPUT);

  if (!probeTouchAddress(kTouchAddressPrimary) &&
      !probeTouchAddress(kTouchAddressAlternate)) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] GT911 not found at 0x5D/0x14");
    return false;
  }

  writeTouchRegister(kTouchStatusRegister, 0);
  g_touch_active = false;
  g_touch_ready = true;
  return true;
}

bool initDisplay() {
  if (g_display_ready) return true;
  if (!initExpander()) return false;

  g_gfx = new WaveshareAtomicRgbDisplay(g_rotation);
  if (!g_gfx || !g_gfx->begin()) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] ST7262 RGB display init failed");
    return false;
  }

  g_gfx->fillScreen(0x0000);
  g_gfx->flush(true);
  g_display_ready = true;
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] Display ready, panel=ST7262, "
      "PCLK=%u MHz, bounce=%u rows/%u px, opt=%s, VSYNC-restart=%u, "
      "PSRAM free=%u KB\n",
      static_cast<unsigned>(kRgbPclkHz / 1000000),
      static_cast<unsigned>(kRgbBounceBufferRows),
      static_cast<unsigned>(kRgbBounceBufferPixels),
      kCompilerOptimization,
      kRestartInVsync ? 1U : 0U,
      static_cast<unsigned>(
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  return true;
}

void mapTouch(uint16_t raw_x, uint16_t raw_y, int16_t& x, int16_t& y) {
  constexpr int16_t kMaxX = 799;
  constexpr int16_t kMaxY = 479;
  if ((g_rotation & 0x03) == 2) {
    x = static_cast<int16_t>(kMaxX - static_cast<int16_t>(raw_x));
    y = static_cast<int16_t>(kMaxY - static_cast<int16_t>(raw_y));
  } else {
    x = static_cast<int16_t>(raw_x);
    y = static_cast<int16_t>(raw_y);
  }
  x = std::max<int16_t>(0, std::min<int16_t>(kMaxX, x));
  y = std::max<int16_t>(0, std::min<int16_t>(kMaxY, y));
}

bool copyFile(fs::FS& src_fs, fs::FS& dst_fs, const char* path) {
  File src = src_fs.open(path, FILE_READ);
  if (!src) return false;
  File dst = dst_fs.open(path, FILE_WRITE);
  if (!dst) {
    src.close();
    return false;
  }
  bool ok = true;
  uint8_t buffer[512];
  while (src.available()) {
    const size_t count = src.read(buffer, sizeof(buffer));
    if (!count) {
      ok = false;
      break;
    }
    if (dst.write(buffer, count) != count) {
      ok = false;
      break;
    }
  }
  dst.close();
  src.close();
  return ok;
}

bool copyDirectory(fs::FS& src_fs, fs::FS& dst_fs, const char* dir_path) {
  // A card without the directory has nothing to migrate.
  if (!src_fs.exists(dir_path)) return true;
  File dir = src_fs.open(dir_path);
  if (!dir || !dir.isDirectory()) return false;
  if (!dst_fs.exists(dir_path) && !dst_fs.mkdir(dir_path)) {
    dir.close();
    return false;
  }

  bool ok = true;
  File entry = dir.openNextFile();
  while (entry) {
    const String path = String(dir_path) + "/" + entry.name();
    const bool directory = entry.isDirectory();
    entry.close();
    if (directory) {
      if (!copyDirectory(src_fs, dst_fs, path.c_str())) ok = false;
    } else if (copyFile(src_fs, dst_fs, path.c_str())) {
      Serial.printf("[Storage] Migrated: %s\n", path.c_str());
    } else {
      ok = false;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return ok;
}

}  // namespace

bool DeviceWaveshareS3TouchLCD7::init() {
  Serial.println(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] Initialising board...");

  if (!psramFound()) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] ERROR: octal PSRAM not detected");
    return false;
  }
  const uint32_t flash_bytes = ESP.getFlashChipSize();
  const uint32_t psram_bytes = ESP.getPsramSize();
  if (flash_bytes != kExpectedFlashBytes ||
      psram_bytes != kExpectedPsramBytes) {
    Serial.printf(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] ERROR: expected N16R8, "
        "detected flash=%u MB, PSRAM=%u MB\n",
        static_cast<unsigned>(flash_bytes / (1024U * 1024U)),
        static_cast<unsigned>(psram_bytes / (1024U * 1024U)));
    return false;
  }
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] Flash=%u MB, PSRAM=%u MB\n",
      static_cast<unsigned>(flash_bytes / (1024U * 1024U)),
      static_cast<unsigned>(psram_bytes / (1024U * 1024U)));

  if (!initExpander()) return false;
  g_backlight_ready = true;
  applyBrightness(0, false);

  // Mount/format LittleFS and create its base directories before the RGB
  // peripheral starts scanning PSRAM. Backlight is already hard-off, so even
  // a long first-install format cannot expose an uninitialised panel.
  if (!initLittleFS()) return false;

  // The first WiFi start after an erased NVS performs a full RF calibration
  // and commits roughly 2 KB of PHY data. Do that one-time operation before
  // RGB/GDMA starts; WiFi.persistent(false) alone only protects WiFi config,
  // not the IDF PHY calibration namespace.
  auto* phy_calibration = static_cast<esp_phy_calibration_data_t*>(
      heap_caps_malloc(sizeof(esp_phy_calibration_data_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  esp_err_t phy_load_err = phy_calibration
                               ? esp_phy_load_cal_data_from_nvs(phy_calibration)
                               : ESP_ERR_NO_MEM;
  if (phy_load_err != ESP_OK) {
    WiFi.persistent(false);
    const bool wifi_started = WiFi.mode(WIFI_STA);
    const bool wifi_stopped = wifi_started && WiFi.mode(WIFI_OFF);
    if (wifi_started && wifi_stopped && phy_calibration) {
      phy_load_err = esp_phy_load_cal_data_from_nvs(phy_calibration);
    }
  }
  if (phy_calibration) heap_caps_free(phy_calibration);

  if (!initDisplay()) return false;

  if (!initTouch()) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] Touch unavailable; continuing");
  }
  return true;
}

void DeviceWaveshareS3TouchLCD7::update() {
  if (g_gfx) g_gfx->service();
}

void DeviceWaveshareS3TouchLCD7::displayPushPixels(
    int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* data) {
  if (!g_display_ready || !g_gfx || !data || w <= 0 || h <= 0) return;
  if (g_turned) {
    g_gfx->drawTurnedArea(x, y, w, h, data, g_rotation);
    return;
  }
  g_gfx->draw16bitRGBBitmap(
      static_cast<int16_t>(x), static_cast<int16_t>(y),
      const_cast<uint16_t*>(data), static_cast<int16_t>(w),
      static_cast<int16_t>(h));
}

void DeviceWaveshareS3TouchLCD7::displayPushPixelsDMA(
    int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* data) {
  displayPushPixels(x, y, w, h, data);
}

bool DeviceWaveshareS3TouchLCD7::displayTryFullFramePreview(
    int32_t x, int32_t y, int32_t w, int32_t h,
    int32_t source_stride, const uint16_t* data, size_t data_size,
    bool byte_swap) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)source_stride;
  (void)data;
  (void)data_size;
  (void)byte_swap;
  return false;
}

bool DeviceWaveshareS3TouchLCD7::displayBeginAtomicFrame(
    const char* reason) {
  return g_display_ready && g_gfx && g_gfx->beginAtomicFrame(reason);
}

void DeviceWaveshareS3TouchLCD7::displayWaitDMA() {}

void DeviceWaveshareS3TouchLCD7::displayFillScreen(uint16_t color) {
  if (g_display_ready && g_gfx) g_gfx->fillScreen(color);
}

void DeviceWaveshareS3TouchLCD7::displaySetRotation(uint8_t rotation) {
  // This panel supports the normal and the 180-degree orientation only.
  g_rotation = rotation & 0x02;
  if (g_display_ready && g_gfx) g_gfx->setRotation(g_rotation);
}

void DeviceWaveshareS3TouchLCD7::displaySetUpright(bool upright) {
  // The turned layout: LVGL's 480x800 screen is drawn a quarter turn into the
  // panel. The touch path needs no second flag: DisplayManager applies the
  // shared turned-layout mapping to this driver's landscape profile point.
  g_turned = upright;
}

void DeviceWaveshareS3TouchLCD7::setBrightness(uint8_t value) {
  applyBrightness(value);
}

uint8_t DeviceWaveshareS3TouchLCD7::getBrightness() {
  return g_brightness;
}

bool DeviceWaveshareS3TouchLCD7::getTouch(int16_t& x, int16_t& y) {
  if (!g_touch_ready) {
    const uint32_t now = millis();
    if (g_touch_init_attempted &&
        (now - g_touch_retry_tick_ms) < kTouchRetryMs) {
      return false;
    }
    if (!initTouch()) return false;
  }

  const auto return_held_point = [&]() {
    if (!g_touch_active) return false;
    x = g_touch_last_x;
    y = g_touch_last_y;
    return true;
  };
  const auto return_held_or_fail_safe = [&](uint8_t& error_streak) {
    if (error_streak < UINT8_MAX) ++error_streak;
    if (!g_touch_active || error_streak < kTouchErrorReleaseThreshold) {
      return return_held_point();
    }
    g_touch_active = false;
    g_touch_status_error_streak = 0;
    g_touch_point_error_streak = 0;
    return false;
  };

  uint8_t status = 0;
  if (!readTouchRegisters(kTouchStatusRegister, &status, 1)) {
    return return_held_or_fail_safe(g_touch_status_error_streak);
  }
  g_touch_status_error_streak = 0;
  if ((status & 0x80) == 0) {
    // Bit 7 means "new coordinate frame ready", not "finger down". Keep the
    // previous active point until GT911 explicitly publishes a ready frame
    // with zero points. This mirrors Espressif's GT911 driver semantics and
    // prevents artificial RELEASE/CLICK cycles between controller reports.
    return return_held_point();
  }

  const uint8_t points = status & 0x0F;
  if (points == 0) {
    writeTouchRegister(kTouchStatusRegister, 0);
    g_touch_active = false;
    g_touch_point_error_streak = 0;
    return false;
  }
  if (points > 5) {
    writeTouchRegister(kTouchStatusRegister, 0);
    return return_held_or_fail_safe(g_touch_point_error_streak);
  }

  uint8_t point[8] = {};
  const bool read_ok =
      readTouchRegisters(kTouchPointRegister, point, sizeof(point));
  writeTouchRegister(kTouchStatusRegister, 0);
  if (!read_ok) {
    return return_held_or_fail_safe(g_touch_point_error_streak);
  }

  const uint16_t raw_x =
      static_cast<uint16_t>(point[1] | (point[2] << 8));
  const uint16_t raw_y =
      static_cast<uint16_t>(point[3] | (point[4] << 8));
  if (raw_x >= 800 || raw_y >= 480) {
    return return_held_or_fail_safe(g_touch_point_error_streak);
  }

  g_touch_point_error_streak = 0;
  mapTouch(raw_x, raw_y, x, y);
  g_touch_last_x = x;
  g_touch_last_y = y;
  if (!g_touch_active) {
    g_touch_active = true;
  }
  return true;
}

void DeviceWaveshareS3TouchLCD7::displaySleep() {
  applyBrightness(0, false);
}

void DeviceWaveshareS3TouchLCD7::displayWake() {
  applyBrightness(g_brightness ? g_brightness : 160, false);
}

void DeviceWaveshareS3TouchLCD7::displayWakeDark() {
  applyBrightness(0, false);
}

void DeviceWaveshareS3TouchLCD7::displayPowerSaveOn() {
  displaySleep();
}

void DeviceWaveshareS3TouchLCD7::displayPowerSaveOff() {
  displayWake();
}

void DeviceWaveshareS3TouchLCD7::displayWaitDisplay() {
  if (g_display_ready && g_gfx) g_gfx->commitAtomicFrame();
}

void DeviceWaveshareS3TouchLCD7::prepareForRestart() {
  applyBrightness(0, false);
  if (g_display_ready && g_gfx) {
    g_gfx->fillScreen(0x0000);
    g_gfx->flush(true);
  }
  delay(20);
}

bool DeviceWaveshareS3TouchLCD7::initSDCard() {
  if (g_sd_available && SD.cardType() != CARD_NONE) return true;

  const uint32_t now = millis();
  if (g_sd_init_attempted && (now - g_sd_retry_tick_ms) < kSdRetryMs) {
    return false;
  }
  g_sd_init_attempted = true;
  g_sd_retry_tick_ms = now;
  if (!g_expander_ready && !initExpander()) return false;
  SD.end();

  // The card select is EXIO4, not a GPIO. Hold it low for every transaction,
  // like Waveshare's Arduino SD example; the card is the only SPI device.
  setExpanderBit(kExpanderSdCs, false);
  delay(2);
  g_sd_spi.begin(kSdSck, kSdMiso, kSdMosi, -1);
  bool mounted = SD.begin(static_cast<uint8_t>(-1), g_sd_spi, kSdFrequency,
                          "/sdcard", 5);
  if (!mounted && kSdFrequency != kSdFallbackFrequency) {
    SD.end();
    g_sd_spi.end();
    delay(2);
    g_sd_spi.begin(kSdSck, kSdMiso, kSdMosi, -1);
    mounted = SD.begin(static_cast<uint8_t>(-1), g_sd_spi,
                       kSdFallbackFrequency, "/sdcard", 5);
  }
  if (!mounted || SD.cardType() == CARD_NONE) {
    if (mounted) SD.end();
    g_sd_available = false;
    setExpanderBit(kExpanderSdCs, true);
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] SD card mount failed");
    return false;
  }

  g_sd_available = true;
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] SD card OK, size=%llu MB\n",
      static_cast<unsigned long long>(SD.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

bool DeviceWaveshareS3TouchLCD7::storageReady() {
  return g_littlefs_ready;
}

fs::FS& DeviceWaveshareS3TouchLCD7::storageFS() {
  return LittleFS;
}

void DeviceWaveshareS3TouchLCD7::storageWriteBegin() {
  if (g_storage_write_depth < UINT16_MAX) {
    ++g_storage_write_depth;
  }
  if (g_storage_write_depth != 1) return;

  // The RGB GDMA reads the PSRAM framebuffer, which is unavailable while the
  // main-flash cache is disabled. Hide the unavoidable RGB underflow behind a
  // backlight blackout and mark the scanout for an explicit restart afterwards.
  if (!g_display_ready) return;

  g_storage_restart_required = true;
  const bool blackout = g_backlight_ready &&
                        (g_backlight_state_unknown ||
                         g_applied_brightness != 0);
  if (blackout) {
    g_storage_blackout_active = true;
    g_storage_restore_brightness = g_applied_brightness;
    applyBrightness(0, false);
    delay(2);
  }

  // IDF's ESP32-S3 restart descriptor is permanently wired to framebuffer 0.
  // Canonicalize while the established storage blackout is active, before the
  // flash cache can be disabled by the caller.
  if (g_gfx && !g_gfx->canonicalizeForStorage()) {
    Serial.println(
        "[Display/S3] Failed to canonicalize framebuffer before flash write");
  }
}

void DeviceWaveshareS3TouchLCD7::storageWriteEnd() {
  if (g_storage_write_depth == 0) return;
  --g_storage_write_depth;
  if (g_storage_write_depth != 0) return;

  const bool restart_required = g_storage_restart_required;
  const bool restore_backlight = g_storage_blackout_active;
  const uint8_t restore_brightness = g_storage_restore_brightness;
  g_storage_restart_required = false;
  g_storage_blackout_active = false;
  g_storage_restore_brightness = 0;

  if (restart_required) {
    uint32_t restart_wait_ms = 0;
    const esp_err_t restart_result =
        g_gfx ? g_gfx->restartAfterStorage(restart_wait_ms)
              : ESP_ERR_INVALID_STATE;
    if (restart_result != ESP_OK) {
      Serial.printf(
          "[Display/S3] RGB restart after flash write failed: %s (0x%X)\n",
          esp_err_to_name(restart_result),
          static_cast<unsigned>(restart_result));
    }

    (void)restart_wait_ms;
  }

  if (restore_backlight) applyBrightness(restore_brightness, false);
}

bool DeviceWaveshareS3TouchLCD7::sdReady() {
  // Mounts happen during board initialisation only; retrying inside UI paths
  // would block the LVGL task for hundreds of milliseconds.
  return g_sd_available && SD.cardType() != CARD_NONE;
}

fs::FS& DeviceWaveshareS3TouchLCD7::sdFS() {
  return SD;
}

bool DeviceWaveshareS3TouchLCD7::suspendSDCardForNetworkTransition() {
  // Native S3 WiFi does not share the P4 ESP-Hosted SDIO bus with the card.
  return false;
}

bool DeviceWaveshareS3TouchLCD7::resumeSDCardAfterNetworkTransition() {
  return initSDCard();
}

bool DeviceWaveshareS3TouchLCD7::initLittleFS() {
  if (g_littlefs_ready) return true;
  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] LittleFS mount failed");
    return false;
  }
  g_littlefs_ready = true;
  ensureStorageLayout();
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] LittleFS ready, total=%u, used=%u\n",
      static_cast<unsigned>(LittleFS.totalBytes()),
      static_cast<unsigned>(LittleFS.usedBytes()));
  return true;
}

void DeviceWaveshareS3TouchLCD7::migrateStorageFromSD() {
  if (!initLittleFS() || LittleFS.exists("/_migrated")) return;

  const bool have_sd = initSDCard();
  storageWriteBegin();
  ensureStorageLayout();
  bool migration_ok = have_sd;
  if (have_sd) {
    Serial.println("[Storage] Migrating data from SD to LittleFS...");
    const bool grids_ok = copyDirectory(SD, LittleFS, "/_tile_grids");
    const bool links_ok = copyDirectory(SD, LittleFS, "/_tile_links");
    migration_ok = grids_ok && links_ok;
    if (migration_ok) {
      Serial.println("[Storage] Migration complete");
    } else {
      Serial.println(
          "[Storage] Migration incomplete; retrying on the next boot");
    }
  } else {
    // Do not write the completion marker: a card inserted or a mount that
    // succeeds after this boot must still be migrated.
    Serial.println(
        "[Storage] No SD card available; migration will retry on the next boot");
  }

  if (migration_ok) {
    File flag = LittleFS.open("/_migrated", FILE_WRITE);
    const bool flag_ok = flag && flag.print("1") == 1;
    flag.close();
    if (!flag_ok) {
      Serial.println(
          "[Storage] Migration marker write failed; retrying on the next boot");
    }
  }
  storageWriteEnd();
}

#endif  // defined(DEVICE_WAVESHARE_S3_TOUCH_LCD_7)
