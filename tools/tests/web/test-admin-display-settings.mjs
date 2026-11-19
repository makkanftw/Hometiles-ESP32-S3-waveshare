// Settings > Display in the Web Admin: brightness, sleep, rotation, the
// screensaver start and its brightness. The page saves each control at once
// through /api/display/settings, like the panel's Display page; the values
// come from the shared settings model, so both surfaces change the same
// configuration and running state.
import assert from 'node:assert/strict';

import {extractFunction, readRepoFile} from '../../lib/admin-source.mjs';

const html = readRepoFile('src/web/server/render/web_admin_html.cpp');
const handlers = readRepoFile('src/web/server/handlers/web_admin_handlers.cpp');
const routes = readRepoFile('src/web/server/web_admin.cpp');
const header = readRepoFile('src/web/server/web_admin.h');
const js = readRepoFile('src/web/admin/settings/display-settings.js');
const bundle = JSON.parse(readRepoFile('src/web/admin/bundle.json'));
const css = readRepoFile('src/web/assets/admin.css');
const docs = readRepoFile('docs/web-admin.md');

const tab = html.slice(html.indexOf('static void appendSettingsTabHtml('),
  html.indexOf('String WebAdminServer::getAdminPage() {'));

// The list follows the panel: Display first, before WiFi.
assert.match(tab,
  /appendSettingsNavItem\(html, "display", "#FFB300", "monitor", tr\.display_label,\s*display_sub\);/,
  'Display is the panel\'s first Settings category');
assert.ok(tab.indexOf('appendSettingsNavItem(html, "display"') <
  tab.indexOf('appendSettingsNavItem(html, "network"'), 'Display leads the list');

// The page: two sections (screen, screensaver) with every control wired to
// the at-once save. No name attribute, so the shared /mqtt form never posts
// the display values with the language and WiFi fields.
const page = tab.slice(tab.indexOf('<section class="settings-page" data-settings-page="display">'),
  tab.indexOf('<section class="settings-page" data-settings-page="network">'));
assert.ok(page.includes('tr.settings_screen') && page.includes('tr.settings_screensaver'),
  'the panel\'s two Display sections');
assert.ok(page.includes('tr.settings_brightness') && page.includes('tr.settings_sleep') &&
  page.includes('tr.settings_rotation') && page.includes('tr.settings_starts_after'),
  'the labels are the central translations');
for (const field of ['display_brightness', 'display_sleep', 'display_rotation',
  'display_saver', 'display_saver_brightness']) {
  assert.ok(page.includes(`id="${field}"`), `${field} is on the page`);
  assert.ok(new RegExp(`id="${field}"(?![^>]*\\bname=)`).test(page),
    `${field} stays out of the /mqtt form`);
}
for (const [field, setting] of [
  ['display_brightness', 'brightness'], ['display_sleep', 'sleep'],
  ['display_rotation', 'rotation'], ['display_saver', 'saver'],
  ['display_saver_brightness', 'saver_brightness'],
]) {
  assert.match(page,
    new RegExp(`id="${field}"[^>]*on(?:input|change)="[^"]*saveDisplaySetting\\('${setting}', this\\.value\\)"`),
    `${field} saves ${setting} on change`);
}
assert.match(page, /appendSettingsStepOptions\(html, settings_model::saved_sleep_index\(\),\s*display_steps, tr\);/,
  'the sleep steps are the panel\'s own options');
assert.match(page, /appendSettingsStepOptions\(html, display_values\.saver_index, display_steps, tr\);/,
  'the screensaver steps are the panel\'s own options');
assert.match(page, /settings_model::quarter_turns\(\)[\s\S]*?rotation_steps = quarter_turns \? 4 : 2;/,
  'quarter-turn panels offer 0/90/180/270, others 0/180');
assert.match(tab, /settingsStepText\(settings_model::saved_sleep_index\(\), display_steps, tr\)/,
  'the category line formats the stored sleep step');
assert.match(tab, /tr\.settings_sleep_summary_fmt[\s\S]*?replace\("%s", display_sleep_step\)/,
  'the category line uses the central summary');
assert.match(tab, /String\(settings_model::saved_brightness\(\)\) \+[\s\S]*?" % \\xC2\\xB7 " \+ display_sleep_summary/,
  'the category line shows brightness and sleep like the panel');

// One route and handler, storage-guarded like the other display settings.
assert.match(routes,
  /server\.on\("\/api\/display\/settings", HTTP_POST,\s*guarded\(withStorageHold\(\[this\]\(\) \{ this->handleSaveDisplaySettings\(\); \}\)\)\);/);
assert.ok(header.includes('void handleSaveDisplaySettings();'));
for (const marker of [
  'const String field = server.arg("field");',
  'String raw = server.arg("value");',
  'const settings_model::DisplayValues display = settings_model::display_values();',
  'value < display.brightness_min || value > display.brightness_max',
  'value < 0 || value > settings_model::sleep_steps()',
  'value < display.saver_brightness_min ||',
  'const int steps = settings_model::quarter_turns() ? 4 : 2;',
  'settings_model::brightness_changed(static_cast<int>(value), true);',
  'settings_model::sleep_changed(static_cast<int>(value), true);',
  'settings_model::saver_changed(static_cast<int>(value), true);',
  'configManager.saveScreensaverBrightness(static_cast<uint8_t>(value))',
  'settings_select_display_rotation(static_cast<uint8_t>(value));',
  'sendJsonError(server, 400, "Unknown display setting");',
]) {
  assert.ok(handlers.includes(marker), `the handler covers: ${marker}`);
}

// Browser behavior: the percent label follows the slider, and a save posts
// the field and value against the display endpoint.
assert.ok(bundle.sources.includes('src/web/admin/settings/display-settings.js'),
  'the Display page script is in the Admin bundle');
const fields = {
  'display_brightness': {value: '73'},
  'display_brightness_value': {textContent: ''},
};
const show = new Function('document', `${extractFunction('showDisplayPercent', js)}
return showDisplayPercent;`)({getElementById: id => fields[id]});
show('display_brightness');
assert.equal(fields['display_brightness_value'].textContent, '73 %',
  'the slider label shows the percent');

const calls = [];
const notify = [];
const save = new Function('fetch', 'showNotification', 't',
  `${extractFunction('saveDisplaySetting', js)}
return saveDisplaySetting;`)(
  async (url, options) => {
    calls.push({url, options});
    return {ok: true, status: 200};
  },
  (message, ok) => notify.push([message, ok]),
  key => key);
await save('saver_brightness', '42');
assert.equal(calls.length, 1);
assert.equal(calls[0].url, '/api/display/settings');
assert.equal(calls[0].options.method, 'POST');
assert.equal(calls[0].options.headers['Content-Type'], 'application/x-www-form-urlencoded');
assert.equal(calls[0].options.body, 'field=saver_brightness&value=42');
assert.deepEqual(notify, [], 'a saved value does not notify');

const failing = new Function('fetch', 'showNotification', 't',
  `${extractFunction('saveDisplaySetting', js)}
return saveDisplaySetting;`)(
  async () => ({ok: false, status: 500}),
  (message, ok) => notify.push([message, ok]),
  key => key);
await failing('brightness', '50');
assert.deepEqual(notify, [['networkErrorSave', false]],
  'a failed save reports through the shared notification');

// Layout: label and percent on one line, the slider across the field.
for (const marker of [
  '.settings-range-field { display:grid; grid-template-columns:minmax(0, 1fr) auto;',
  '.settings-range-field output { grid-row:1; grid-column:2;',
  '.settings-range-field input[type="range"] { grid-row:2; grid-column:1 / -1;',
]) assert.ok(css.includes(marker), `the Display slider layout: ${marker}`);

// Docs list the page with the panel's Display controls.
assert.ok(docs.includes('- **Display:** brightness, sleep, rotation, when the screensaver starts, and its brightness.'),
  'web-admin.md lists Display');

console.log('Web Admin Display settings: page, endpoint and browser behavior passed');
