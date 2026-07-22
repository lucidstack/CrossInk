#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

// Bluetooth settings tab: scan for BLE HID keyboards, connect/pair, disconnect,
// and unpair (forget). Brings NimBLE up on entry (after freeing the SD reading
// font for heap headroom) and tears it down on exit via BleHid.end(). Reached
// from Settings > Bluetooth. Bonding is Just Works (no passkey entry needed for
// the common keyboard case; a displayed passkey is surfaced if the peer asks).
//
// Guarded by EDITOR_BLE_SPIKE (needs FREEINK_CAP_BLE_HID_HOST), like the editor.
class BluetoothSettingsActivity final : public Activity {
 public:
  BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bluetooth", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }  // keep awake while pairing

 private:
  enum class Kind { Rescan, Bonded, Discovered };
  struct Entry {
    Kind kind = Kind::Rescan;
    std::string name;
    std::string addr;
    bool connected = false;  // bonded entry that is the live link
  };

  size_t selectorIndex = 0;
  std::vector<Entry> entries_;

  bool bleReady_ = false;
  bool lastConnected_ = false;
  uint8_t lastDeviceCount_ = 0;
  uint8_t lastBondCount_ = 0;
  unsigned long scanStartedAt_ = 0;

  void startScan();
  void rebuildEntries();     // recompute the [Rescan] + bonded + discovered list
  void actOnSelected();      // Confirm: connect / disconnect / pair / rescan
  void promptForgetSelected();  // Left: unpair a bonded device (with confirmation)
};
