// Spike-only translation unit: BleKeyboardHost (and NimBLE) exist only in the
// spike env, and PlatformIO compiles every src/**.cpp in every env.
#ifdef EDITOR_BLE_SPIKE

#include "BluetoothSettingsActivity.h"

#include <BleKeyboardHost.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <functional>

#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/CompactHeader.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t kScanMs = 8000;
}  // namespace

void BluetoothSettingsActivity::startScan() {
  if (!bleReady_) return;
  BleHid.startScan(kScanMs);
  scanStartedAt_ = millis();
  requestUpdate();
}

void BluetoothSettingsActivity::rebuildEntries() {
  entries_.clear();
  entries_.push_back({Kind::Rescan, tr(STR_BT_SCAN), "", false});

  const bool linked = bleReady_ && BleHid.isConnected();
  const char* linkedName = linked ? BleHid.connectedName() : "";

  // Bonded devices first (persisted; can connect/disconnect/forget).
  for (uint8_t i = 0; bleReady_ && i < BleHid.pairedCount(); i++) {
    const auto& b = BleHid.paired(i);
    const bool isLink = linked && linkedName[0] != '\0' && std::string(linkedName) == b.name;
    entries_.push_back({Kind::Bonded, b.name[0] ? b.name : b.addr, b.addr, isLink});
  }

  // Discovered HID devices not already bonded.
  for (uint8_t i = 0; bleReady_ && i < BleHid.deviceCount(); i++) {
    const auto& d = BleHid.device(i);
    if (!d.hid || !d.connectable) continue;
    bool bonded = false;
    for (uint8_t j = 0; j < BleHid.pairedCount(); j++) {
      if (std::string(BleHid.paired(j).addr) == d.addr) {
        bonded = true;
        break;
      }
    }
    if (bonded) continue;
    entries_.push_back({Kind::Discovered, d.hasName ? d.name : d.addr, d.addr, false});
  }

  if (selectorIndex >= entries_.size()) selectorIndex = entries_.empty() ? 0 : entries_.size() - 1;
}

void BluetoothSettingsActivity::actOnSelected() {
  if (selectorIndex >= entries_.size()) return;
  const Entry e = entries_[selectorIndex];
  switch (e.kind) {
    case Kind::Rescan:
      startScan();
      return;
    case Kind::Bonded:
      if (e.connected) {
        BleHid.disconnect();
      } else {
        BleHid.stopScan();  // free the radio + scan RAM before connecting (heap)
        BleHid.connect(e.addr.c_str());
        BleHid.releaseScanResults();
      }
      requestUpdate();
      return;
    case Kind::Discovered:
      BleHid.stopScan();
      BleHid.connect(e.addr.c_str());  // Just Works pairing bonds on success
      BleHid.releaseScanResults();
      requestUpdate();
      return;
  }
}

void BluetoothSettingsActivity::promptForgetSelected() {
  if (selectorIndex >= entries_.size()) return;
  const Entry e = entries_[selectorIndex];
  if (e.kind != Kind::Bonded) return;  // only bonded devices can be forgotten

  const std::string addr = e.addr;
  const std::string heading = tr(STR_BT_FORGET) + std::string("? ");
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, e.name),
      [this, addr](const ActivityResult& res) {
        if (res.isCancelled) return;
        BleHid.forget(addr.c_str());
        rebuildEntries();
        requestUpdate();
      });
}

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  // Free the SD reading font so NimBLE init has a large contiguous block (same
  // heap fix as the editor); the reader reloads it on demand.
  sdFontSystem.releaseForNetwork(renderer);

  bleReady_ = BleHid.begin("CrossInk");
  if (!bleReady_) {
    LOG_ERR("BT", "BleHid.begin() failed — capability compiled out or BLE init error");
  } else {
    lastBondCount_ = BleHid.pairedCount();
    startScan();  // surface nearby keyboards immediately
  }
  rebuildEntries();
  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  BleHid.end();  // tear down NimBLE so its heap returns to the reader/home
  Activity::onExit();
}

void BluetoothSettingsActivity::loop() {
  if (bleReady_) BleHid.poll();

  using Btn = MappedInputManager::Button;

  if (mappedInput.wasReleased(Btn::Back)) {
    activityManager.goToSettings();  // BT tab is a stack base; return to Settings
    return;
  }
  if (mappedInput.wasReleased(Btn::Confirm)) {
    actOnSelected();
    return;
  }
  if (mappedInput.wasReleased(Btn::Right)) {  // rescan
    startScan();
    return;
  }
  if (mappedInput.wasReleased(Btn::Left)) {  // forget the selected bonded device
    promptForgetSelected();
    return;
  }
  if (mappedInput.wasReleased(Btn::Down)) {
    if (!entries_.empty()) selectorIndex = (selectorIndex + 1) % entries_.size();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(Btn::Up)) {
    if (!entries_.empty()) selectorIndex = (selectorIndex + entries_.size() - 1) % entries_.size();
    requestUpdate();
    return;
  }

  // React to BLE state changes (scan results arriving, link up/down, new bonds).
  if (bleReady_) {
    const bool connected = BleHid.isConnected();
    const uint8_t devCount = BleHid.deviceCount();
    const uint8_t bondCount = BleHid.pairedCount();
    if (connected != lastConnected_ || devCount != lastDeviceCount_ || bondCount != lastBondCount_) {
      lastConnected_ = connected;
      lastDeviceCount_ = devCount;
      lastBondCount_ = bondCount;
      rebuildEntries();
      requestUpdate();
    }
  }
}

void BluetoothSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // Title + status subtitle (connected device / scanning / idle).
  std::string subtitle;
  if (!bleReady_) {
    subtitle = tr(STR_BT_UNAVAILABLE);
  } else if (BleHid.isConnected()) {
    subtitle = std::string(tr(STR_BT_CONNECTED)) + ": " + BleHid.connectedName();
  } else if (BleHid.isScanning()) {
    subtitle = tr(STR_BT_SCANNING);
  } else {
    subtitle = tr(STR_BT_NOT_CONNECTED);
  }
  CompactHeader::drawTitle(renderer, tr(STR_BLUETOOTH));

  // Status subtitle drawn just under the header; the list starts below it.
  const int headerTop = CompactHeader::contentTop(metrics);
  const int subtitleH = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, headerTop, subtitle.c_str());

  const int contentTop = headerTop + subtitleH + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const auto rowTitle = [this](int index) -> std::string {
    const Entry& e = entries_[index];
    if (e.kind == Kind::Rescan) return BleHid.isScanning() ? tr(STR_BT_SCANNING) : tr(STR_BT_SCAN);
    return e.name;
  };
  const auto rowValue = [this](int index) -> std::string {
    const Entry& e = entries_[index];
    if (e.connected) return tr(STR_BT_CONNECTED);
    if (e.kind == Kind::Bonded) return tr(STR_BT_PAIRED);
    return "";
  };

  const Rect listRect{0, contentTop, pageWidth, contentHeight};
  GUI.drawList(renderer, listRect, static_cast<int>(entries_.size()), static_cast<int>(selectorIndex), rowTitle,
               nullptr, nullptr, rowValue);

  // Legend: Back, Confirm=Connect/Disconnect, Left=Forget, Right=Scan.
  const bool onBonded = selectorIndex < entries_.size() && entries_[selectorIndex].kind == Kind::Bonded;
  const char* confirmLabel =
      (onBonded && entries_[selectorIndex].connected) ? tr(STR_BT_DISCONNECT) : tr(STR_CONNECT);
  const char* forgetLabel = onBonded ? tr(STR_BT_FORGET) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, forgetLabel, tr(STR_BT_SCAN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // EDITOR_BLE_SPIKE
