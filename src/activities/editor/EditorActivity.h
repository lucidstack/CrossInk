#pragma once

#include <string>

#include "activities/Activity.h"

// Real text-editor screen for CrossInk. Drives the ported MicroSlate editor
// core (src/editor/text_editor.cpp) — word wrap, cursor, markdown styling — and
// takes input from a BLE HID keyboard via the freeink-sdk BleKeyboardHost.
//
// Reached from the Home "Notes" item: FileBrowserActivity (Mode::PickNote)
// returns a chosen note path, or an empty path for "New note". Launch as a
// stack base via replaceActivity (NOT pushActivity — a sole pushed activity
// leaves a null stack base that the sleep teardown dereferences).
//
// Notes live under /notes/ as .md. Save is explicit (Ctrl+S) plus save-on-exit;
// autosave is a later step.
class EditorActivity final : public Activity {
 public:
  // filePath: absolute path of a note to open, or "" to start a new blank note.
  EditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath = "")
      : Activity("Editor", renderer, mappedInput), filePath_(std::move(filePath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }  // keep awake while editing

 private:
  static constexpr const char* kNotesDir = "/notes";

  std::string filePath_;  // "" until a new note is first saved
  bool justSaved_ = false;

  bool connectAttempted_ = false;
  bool lastConnected_ = false;
  unsigned long scanStartedAt_ = 0;

  void loadNote();       // read filePath_ into the editor buffer (if it exists)
  bool saveNote();       // write the buffer back to SD; assigns filePath_ for new notes
  void drainKeyboard();  // pump BLE key events into the editor core
};
