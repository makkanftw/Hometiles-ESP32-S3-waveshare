// The 800x480 Waveshare S3 panel's popup/setup card is 480x480 px, the same
// as the 480x480 class. Its Settings pages, setup and WiFi keyboard must use
// that compact geometry: at 480 px high the large-panel rows leave no room
// for the network list and the keyboard covers the password field.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const read = relativePath => fs.readFileSync(path.join(repoRoot, relativePath), 'utf8')
  .replace(/\r\n?/g, '\n');

const compactCondition =
  'defined(DEVICE_LAYOUT_480X480) || defined(DEVICE_WAVESHARE_S3_TOUCH_LCD_7)';
for (const file of [
  'src/fonts/ui_fonts.h',
  'src/tiles/config/grid_layout.h',
  'src/ui/popups/popup_layout.h',
  'src/ui/tabs/settings/settings_style.h',
  'src/ui/tabs/settings/settings_entry.cpp',
  'src/ui/tabs/settings/settings_keyboard.cpp',
  'src/ui/tabs/settings/settings_screen.cpp',
  'src/ui/tabs/settings/setup_screen.cpp',
]) {
  assert.ok(read(file).includes(compactCondition),
    `${file}: the S3 7-inch panel must use the compact UI class`);
}

for (const file of ['src/fonts/ui_font_12.c', 'src/fonts/ui_font_14.c']) {
  assert.ok(read(file).includes(compactCondition),
    `${file}: the compact font must be compiled for the S3 7-inch panel`);
}

const style = read('src/ui/tabs/settings/settings_style.h');
const entryStart = style.indexOf('constexpr int kEntryPad');
assert.ok(entryStart > 0, 'the compact entry geometry was not found');
const entryBlock = style.slice(entryStart, style.indexOf('#endif', entryStart));
const number = name => {
  const match = entryBlock.match(new RegExp(`constexpr int ${name} = (\\d+);`));
  assert.ok(match, `compact constant ${name} was not found`);
  return Number(match[1]);
};
const entryPad = number('kEntryPad');
const fieldTop = number('kSetupFieldTop');
const fieldHeight = number('kSetupFieldHeight');
const messageTop = number('kSetupMessageTop');
const keyHeight = number('kKeyHeight');
const keyStep = number('kKeyStep');
const keyboardBottom = number('kKeyboardBottom');

assert.equal(entryPad, 13);
assert.equal(fieldTop, 76);
assert.equal(fieldHeight, 48);
assert.equal(messageTop, 132);
assert.equal(keyHeight, 56);
assert.equal(keyStep, 62);
assert.equal(keyboardBottom, 13);

const cardHeight = 480;  // the setup card fills the 800x480 screen's height
const keyboardTop = cardHeight - keyboardBottom - (3 * keyStep + keyHeight);
assert.equal(keyboardTop, 225, 'the compact keyboard starts below the fields');
assert.ok(fieldTop + fieldHeight < keyboardTop,
  `password field ${fieldTop}..${fieldTop + fieldHeight} must end above keyboard ${keyboardTop}`);
assert.ok(messageTop < keyboardTop, 'the message line stays above the keyboard');
assert.ok(keyboardTop + 3 * keyStep + keyHeight + keyboardBottom <= cardHeight,
  'the compact keyboard fits the 480 px card');

const pickSmall = name => {
  const match = style.match(new RegExp(`constexpr int ${name} = pick\\((\\d+), (\\d+)\\);`));
  assert.ok(match, `${name} was not found`);
  return Number(match[2]);
};
assert.equal(pickSmall('kRowHeight'), 59, 'compact Settings rows');
assert.equal(pickSmall('kSetupRowHeight'), 66, 'compact setup rows');

// The WiFi list sets its own scroll height when its rows are taller than the
// body. With 66 px setup rows a 480 px card leaves several rows of room, so
// the list becomes scrollable instead of hanging outside the card.
assert.ok(pickSmall('kSetupRowHeight') < 100, 'the setup list rows are compact');

const popup = read('src/ui/popups/popup_layout.h');
assert.match(popup, /#elif \(defined\(DEVICE_LAYOUT_480X480\) \|\| defined\(DEVICE_WAVESHARE_S3_TOUCH_LCD_7\)\)\n  return \(value \* 2 \+ \(value >= 0 \? 1 : -1\)\) \/ 3;/,
  'the compact popup scale applies to the S3 7-inch panel');
assert.match(popup, /#elif \(defined\(DEVICE_LAYOUT_480X480\) \|\| defined\(DEVICE_WAVESHARE_S3_TOUCH_LCD_7\)\)\nconstexpr int kHeaderCenterY = Device::kGridGap \+ 64 \/ 2;/,
  'the compact popup head applies to the S3 7-inch panel');
assert.match(read('src/tiles/config/grid_layout.h'),
  /#if \(defined\(DEVICE_LAYOUT_480X480\) \|\| defined\(DEVICE_WAVESHARE_S3_TOUCH_LCD_7\)\)\nconstexpr int kHeadClose = 64;/,
  'the head bar X matches the compact popup close button');

console.log('Settings compact UI for the Waveshare S3 7-inch: PASS');
