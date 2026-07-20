#include "EditorSpikeActivity.h"

#include <BleKeyboardHost.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "fontIds.h"

using freeink::KeyEvent;
using freeink::SpecialKey;

namespace {
constexpr int kBodyFont = UI_12_FONT_ID;
constexpr int kUiFont = UI_10_FONT_ID;
constexpr size_t kMaxChars = 4096;  // spike cap; the real editor uses a proper buffer
}  // namespace

void EditorSpikeActivity::onEnter() {
  Activity::onEnter();

  if (!BleHid.begin("CrossInk")) {
    LOG_ERR("SPIKE", "BleHid.begin() failed — capability compiled out or BLE init error");
  } else if (BleHid.pairedCount() > 0) {
    // poll() drives auto-reconnect to a known bond.
    LOG_INF("SPIKE", "BLE up; %u bond(s) known, waiting for auto-reconnect", BleHid.pairedCount());
  } else {
    LOG_INF("SPIKE", "BLE up; no bonds, scanning for a keyboard");
    BleHid.startScan(8000);
    scanStartedAt_ = millis();
  }
  requestUpdate();
}

void EditorSpikeActivity::loop() {
  BleHid.poll();

  // Physical Back clears the buffer so a spike session is easy to reset.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    buffer_.clear();
    requestUpdate();
  }

  // One connect attempt at the first HID device a scan turns up.
  if (!connectAttempted_ && !BleHid.isConnected() && !BleHid.isConnecting() && !BleHid.isScanning() &&
      BleHid.deviceCount() > 0) {
    for (uint8_t i = 0; i < BleHid.deviceCount(); i++) {
      const auto& d = BleHid.device(i);
      if (d.hid && d.connectable) {
        LOG_INF("SPIKE", "Connecting to %s (%s)", d.name, d.addr);
        BleHid.connect(d.addr);
        connectAttempted_ = true;
        requestUpdate();
        break;
      }
    }
    if (!connectAttempted_) {
      // Nothing HID-shaped in this scan; sweep again.
      BleHid.startScan(8000);
      scanStartedAt_ = millis();
    }
  }

  // Surface connection state changes (link comes up on the NimBLE host task).
  if (BleHid.isConnected() != lastConnected_) {
    lastConnected_ = BleHid.isConnected();
    LOG_INF("SPIKE", "Link %s%s", lastConnected_ ? "UP: " : "DOWN", lastConnected_ ? BleHid.connectedName() : "");
    requestUpdate();
  }

  // Drain translated key events into the buffer.
  bool dirty = false;
  KeyEvent ev;
  while (BleHid.popKey(ev)) {
    if (ev.special == SpecialKey::Backspace) {
      if (!buffer_.empty()) buffer_.pop_back();
      dirty = true;
    } else if (ev.special == SpecialKey::Enter) {
      buffer_.push_back('\n');
      dirty = true;
    } else if (ev.ch >= 32 && ev.ch < 127) {
      if (buffer_.size() < kMaxChars) buffer_.push_back(ev.ch);
      dirty = true;
    }
    // Other specials (arrows/etc.) ignored in the spike.
  }
  if (dirty) requestUpdate();
}

void EditorSpikeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int sw = renderer.getScreenWidth();
  const int uiH = renderer.getLineHeight(kUiFont);
  const int bodyH = renderer.getLineHeight(kBodyFont);

  renderer.drawText(kUiFont, 10, 4, "BLE Editor Spike", true, EpdFontFamily::BOLD);

  const char* status;
  if (BleHid.isConnected()) status = BleHid.connectedName();
  else if (BleHid.isConnecting()) status = "connecting...";
  else if (BleHid.isScanning()) status = "scanning...";
  else status = "no keyboard";
  renderer.drawText(kUiFont, 10, 4 + uiH, status, true, EpdFontFamily::REGULAR);

  // Greedy character wrap of the buffer within the screen width.
  int y = 8 + 2 * uiH;
  const int maxY = renderer.getScreenHeight() - bodyH;
  const int leftX = 10;
  const int rightX = sw - 10;
  std::string line;
  auto flush = [&]() {
    renderer.drawText(kBodyFont, leftX, y, line.c_str(), true, EpdFontFamily::REGULAR);
    y += bodyH;
    line.clear();
  };
  for (char c : buffer_) {
    if (c == '\n') {
      if (y > maxY) break;
      flush();
      continue;
    }
    std::string trial = line + c;
    if (renderer.getTextWidth(kBodyFont, trial.c_str()) > (rightX - leftX)) {
      if (y > maxY) break;
      flush();
    }
    line.push_back(c);
  }
  if (y <= maxY) {
    // Draw the final line plus a block caret at its end.
    renderer.drawText(kBodyFont, leftX, y, line.c_str(), true, EpdFontFamily::REGULAR);
    const int caretX = leftX + renderer.getTextWidth(kBodyFont, line.c_str());
    renderer.fillRect(caretX + 1, y, 3, bodyH - 2, true);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
