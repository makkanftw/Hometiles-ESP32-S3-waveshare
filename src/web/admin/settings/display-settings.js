// Settings > Display in the Web Admin: brightness, sleep, rotation, when the
// screensaver starts and its brightness. Every control applies and saves at
// once, like the panel's Display page (settings_model, tab_settings.cpp).
// Sliders show their value while the thumb moves and save on release; the
// server validates every value and reports the stored result.

function showDisplayPercent(id) {
  const input = document.getElementById(id);
  const output = document.getElementById(id + '_value');
  if (input && output) output.textContent = input.value + ' %';
}

async function saveDisplaySetting(field, value) {
  try {
    const response = await fetch('/api/display/settings', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'field=' + encodeURIComponent(field) + '&value=' + encodeURIComponent(value)
    });
    if (!response.ok) throw new Error('HTTP ' + response.status);
  } catch (error) {
    showNotification(t('networkErrorSave'), false);
  }
}
