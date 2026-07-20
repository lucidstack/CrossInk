#pragma once

#include "activities/Activity.h"

// Real text-editor screen for CrossInk. Drives the ported MicroSlate editor
// core (src/editor/text_editor.cpp) — word wrap, cursor, markdown styling — and
// takes input from a BLE HID keyboard via the freeink-sdk BleKeyboardHost.
//
// Step 2 of the port: render + BLE input. File load/save (SDCardManager) is
// step 3; until then the buffer starts empty and is not persisted. Launch as a
// stack base via replaceActivity (NOT pushActivity — a sole pushed activity
// leaves a null stack base that the sleep teardown dereferences).
class EditorActivity final : public Activity {
 public:
  EditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Editor", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }  // keep awake while editing

 private:
  bool connectAttempted_ = false;
  bool lastConnected_ = false;
  unsigned long scanStartedAt_ = 0;

  void drainKeyboard();  // pump BLE key events into the editor core
};
