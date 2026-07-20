#pragma once
#include <string>

#include "activities/Activity.h"

// Throwaway spike (guarded by -DEDITOR_BLE_SPIKE) proving the freeink-sdk
// BleKeyboardHost comes up on CrossInk's stack: scan -> connect an HID
// keyboard -> render typed characters. NOT a real editor — no file I/O, no
// markdown, minimal wrapping. If this types on screen, the full editor port
// is de-risked.
class EditorSpikeActivity final : public Activity {
 public:
  EditorSpikeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("EditorSpike", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }  // keep awake while spiking

 private:
  std::string buffer_;
  bool connectAttempted_ = false;
  bool lastConnected_ = false;
  unsigned long scanStartedAt_ = 0;
};
