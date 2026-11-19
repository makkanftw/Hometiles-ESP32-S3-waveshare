// The 800x480 Waveshare S3 panel is landscape by itself. The upright layout
// draws a 480x800 LVGL screen and the driver turns every flushed area a
// quarter into the framebuffer. This checks the exact place formula against
// the shared DisplayManager touch turn for both supported 180-degree
// rotations.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const read = relativePath => fs.readFileSync(path.join(repoRoot, relativePath), 'utf8')
  .replace(/\r\n?/g, '\n');

const driver = read('src/devices/waveshare_s3_touch_lcd_7/device_waveshare_s3_touch_lcd_7.cpp');
const grid = read('src/tiles/config/grid_layout.h');
const facade = read('src/devices/device.cpp');

assert.match(grid, /#if defined\(DEVICE_GUITION_JC8012P4A1\)[\s\S]*?defined\(DEVICE_WAVESHARE_S3_TOUCH_LCD_7\)\n  return true;[\s\S]*?constexpr bool turn_reversed\(\)/,
  'the S3 7-inch panel is upright-ready');
assert.match(grid, /constexpr bool turn_reversed\(\) \{[\s\S]*?defined\(DEVICE_WAVESHARE_S3_TOUCH_LCD_7\)\n  return true;/,
  'the S3 7-inch panel turns like the other landscape-native panels');
assert.match(facade, /#if defined\(DEVICE_GUITION_JC8012P4A1\)[\s\S]*?defined\(DEVICE_WAVESHARE_S3_TOUCH_LCD_7\)\n  DeviceImpl::displaySetUpright\(upright\);/,
  'the facade dispatches displaySetUpright to the new driver');

for (const marker of [
  '#include "src/devices/common/p4_dsi_cpu_rotate.h"',
  'bool g_turned = false;',
  'void drawTurnedArea(int32_t x, int32_t y, int32_t w, int32_t h,',
  'if (g_turned) {\n    g_gfx->drawTurnedArea(x, y, w, h, data, g_rotation);\n    return;\n  }',
  'void DeviceWaveshareS3TouchLCD7::displaySetUpright(bool upright) {',
  'g_turned = upright;',
  'p4_dsi_cpu_rotate::rotate_into(\n        _framebuffer, _fb_width, dst_x, dst_y, w, h, data,\n        (turn & 0x02) != 0);',
]) {
  assert.ok(driver.includes(marker), `driver is missing: ${marker.split('\n')[0]}`);
}

const dst = /    const int32_t dst_x = \(turn & 0x02\) \? y : panel_w - y - h;\n    const int32_t dst_y = \(turn & 0x02\) \? panel_h - x - w : x;\n/.exec(driver);
assert.ok(dst, 'the turned destination formula was not found');

const compiler = ['clang++', 'g++'].find(candidate =>
  spawnSync(candidate, ['--version']).status === 0);
if (!compiler) {
  console.log('SKIP: the native upright mapping check needs a C++ compiler');
} else {
  const rotate = read('src/devices/common/p4_dsi_cpu_rotate.h')
    .replace('#pragma once', '');
  const source = `#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
${rotate}
constexpr int32_t kUiW = 480, kUiH = 800;
constexpr int32_t kPanelW = 800, kPanelH = 480;
void turned_dst(uint8_t turn, int32_t x, int32_t y, int32_t w, int32_t h,
                int32_t& out_x, int32_t& out_y) {
  const int32_t panel_w = kPanelW, panel_h = kPanelH;
${dst[0]}  out_x = dst_x;
  out_y = dst_y;
}
// The driver's getTouch(): the panel point in the profile's landscape axes,
// with the 180-degree setting mirrored.
void driver_touch(uint8_t rotation, int32_t px, int32_t py,
                  int16_t& x, int16_t& y) {
  if (rotation & 0x02) {
    x = static_cast<int16_t>(kPanelW - 1 - px);
    y = static_cast<int16_t>(kPanelH - 1 - py);
  } else {
    x = static_cast<int16_t>(px);
    y = static_cast<int16_t>(py);
  }
}
// DisplayManager::touch_cb(): the shared turned-layout mapping.
void central_turn(int16_t& x, int16_t& y) {
  const int16_t landscape_x = x;
  x = static_cast<int16_t>(kUiW - 1 - y);
  y = landscape_x;
}
int main() {
  std::vector<uint16_t> fb(static_cast<size_t>(kPanelW) * kPanelH, 0xFFFF);
  for (int32_t ux : {0, 1, 239, 478, 479}) {
    for (int32_t uy : {0, 1, 399, 798, 799}) {
      for (uint8_t rotation : {0, 2}) {
        std::fill(fb.begin(), fb.end(), 0xFFFF);
        const uint8_t turn = rotation ^ 0x02;
        int32_t dst_x = 0, dst_y = 0;
        turned_dst(turn, ux, uy, 1, 1, dst_x, dst_y);
        const uint16_t marker = static_cast<uint16_t>(0x8000 | ((ux + uy) & 0x7FFF));
        p4_dsi_cpu_rotate::rotate_into(fb.data(), kPanelW, dst_x, dst_y, 1, 1,
                                       &marker, (turn & 0x02) != 0);
        int found = 0;
        int32_t fx = 0, fy = 0;
        for (int32_t py = 0; py < kPanelH; ++py) {
          for (int32_t px = 0; px < kPanelW; ++px) {
            if (fb[static_cast<size_t>(py) * kPanelW + px] == marker) {
              ++found;
              fx = px;
              fy = py;
            }
          }
        }
        assert(found == 1);
        int16_t x = 0, y = 0;
        driver_touch(rotation, fx, fy, x, y);
        central_turn(x, y);
        assert(x == ux && y == uy);
      }
    }
  }
  std::printf("upright mapping ok\\n");
  return 0;
}
`;
  const temp = fs.mkdtempSync('/tmp/hometiles-s3-7-upright-');
  const cpp = path.join(temp, 'test.cpp');
  const exe = path.join(temp, 'test');
  fs.writeFileSync(cpp, source);
  const build = spawnSync(compiler, ['-std=c++17', '-O1', '-Wall', '-Wextra', '-o', exe, cpp],
    { encoding: 'utf8' });
  assert.equal(build.status, 0, build.stderr || build.stdout);
  const run = spawnSync(exe, [], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr || run.stdout);
  assert.match(run.stdout, /upright mapping ok/);
  fs.rmSync(temp, { recursive: true, force: true });
}

console.log('Waveshare ESP32-S3-Touch-LCD-7 upright layout: PASS');
