#pragma once

#include <cstddef>

// Minimal build-time constants for the ported MicroSlate text editor core.
// The full MicroSlate config.h carried UI/BLE/HID state the editor buffer does
// not touch; only the buffer/line/name sizes below are needed to compile and
// test text_editor.cpp host-side. Keep this decoupled from CrossInk's own
// config so the editor core stays a pure, independently testable unit.

static constexpr size_t TEXT_BUFFER_SIZE = 16384;
static constexpr int MAX_LINES = 1024;
static constexpr int MAX_FILENAME_LEN = 64;
static constexpr int MAX_TITLE_LEN = 40;
