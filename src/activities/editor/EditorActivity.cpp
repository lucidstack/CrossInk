// Spike-only translation unit: BleKeyboardHost (and NimBLE) exist only in the
// spike env, and PlatformIO compiles every src/**.cpp in every env.
#ifdef EDITOR_BLE_SPIKE

#include "EditorActivity.h"

#include <BleKeyboardHost.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "editor/markdown.h"
#include "editor/note_names.h"
#include "editor/text_editor.h"
#include "fontIds.h"

using freeink::KeyEvent;
using freeink::SpecialKey;

namespace {
// Body-font size table cycled by the on-device Up/Down keys. LEXENDDECA is a
// proper reading typeface with bold/italic faces (so markdown styling shows).
// applyFontIndex() falls back to UI_12 if a size isn't compiled into this build.
constexpr int g_bodyFontSizes[] = {LEXENDDECA_12_FONT_ID, LEXENDDECA_14_FONT_ID, LEXENDDECA_16_FONT_ID};
constexpr int g_bodyFontCount = static_cast<int>(sizeof(g_bodyFontSizes) / sizeof(g_bodyFontSizes[0]));
int g_bodyFont = LEXENDDECA_14_FONT_ID;  // current body font (mutable; see applyFontIndex)

constexpr int kUiFont = UI_10_FONT_ID;
constexpr int kMargin = 10;

// The button legend always draws along the physical bottom edge where the four
// buttons sit (drawButtonHints forces Portrait internally), so after an
// editor-local rotation the strip lands on a different logical edge. Map it so
// layout can reserve that space instead of rendering text under the legend.
void legendInsets(const GfxRenderer& renderer, const int strip, int& top, int& right, int& bottom, int& left) {
  top = right = bottom = left = 0;
  switch (renderer.getOrientation()) {
    case GfxRenderer::Orientation::Portrait:
      bottom = strip;
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      left = strip;
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      top = strip;
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      right = strip;
      break;
  }
}

int legendStrip() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.buttonHintsHeight + metrics.verticalSpacing;
}

// Draw one wrapped editor line at (x, yPos) with markdown styling. Headings
// (leading "# "/"## "/"### ") render bold; **bold**/*italic* runs toggle inline.
// Only the first visual line of a wrapped heading is bold — the wrap math keeps
// a uniform line height, so this is the intended tradeoff carried over from
// MicroSlate.
void drawEditorLine(GfxRenderer& renderer, int lineIdx, int x, int yPos) {
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();
  int totalLines = editorGetLineCount();

  int lineStart = editorGetLinePosition(lineIdx);
  int lineEnd = (lineIdx + 1 < totalLines) ? editorGetLinePosition(lineIdx + 1) : (int)bufLen;
  int dispEnd = lineEnd;
  if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

  int len = dispEnd - lineStart;
  if (len <= 0) return;

  char lineBuf[256];
  int copyLen = (len < (int)sizeof(lineBuf) - 1) ? len : (int)sizeof(lineBuf) - 1;
  strncpy(lineBuf, buf + lineStart, copyLen);
  lineBuf[copyLen] = '\0';

  int headingLevel = mdHeadingLevel(lineBuf, copyLen);
  uint8_t baseStyle = headingLevel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

  MdRun runs[24];
  int runCount = mdParseInline(lineBuf, copyLen, baseStyle, runs, 24);

  int xCur = x;
  char runBuf[256];
  for (int r = 0; r < runCount; r++) {
    memcpy(runBuf, lineBuf + runs[r].start, runs[r].len);
    runBuf[runs[r].len] = '\0';
    renderer.drawText(g_bodyFont, xCur, yPos, runBuf, true, runs[r].style);
    xCur += renderer.getTextWidth(g_bodyFont, runBuf, runs[r].style);
  }

  // H1 gets an underline for hierarchy without breaking the uniform line height.
  if (headingLevel == 1) {
    int underlineY = yPos + renderer.getFontAscenderSize(g_bodyFont) + 3;
    if (xCur > x) renderer.drawLine(x, underlineY, xCur, underlineY, true);
  }
}

// Pixel width of the first prefixLen bytes of a line using the same
// heading/inline styling as drawEditorLine, so the caret lands where the styled
// glyphs actually end. Runs are parsed over the FULL line (a prefix-only parse
// would mis-style text after an opening marker that closes past the cursor),
// then only the prefix portion is measured.
int measureStyledPrefix(GfxRenderer& renderer, const char* lineBuf, int lineLen, int prefixLen, uint8_t baseStyle) {
  MdRun runs[24];
  int runCount = mdParseInline(lineBuf, lineLen, baseStyle, runs, 24);

  int w = 0;
  char runBuf[256];
  for (int r = 0; r < runCount && runs[r].start < prefixLen; r++) {
    int take = runs[r].len;
    if (runs[r].start + take > prefixLen) take = prefixLen - runs[r].start;
    memcpy(runBuf, lineBuf + runs[r].start, take);
    runBuf[take] = '\0';
    w += renderer.getTextWidth(g_bodyFont, runBuf, runs[r].style);
  }
  return w;
}

void drawEditorCursor(GfxRenderer& renderer, int cursorY, int lineHeight, int textX, int textRight) {
  int curLine = editorGetCursorLine();
  int curCol = editorGetCursorCol();
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();
  int totalLines = editorGetLineCount();

  int lineStart = editorGetLinePosition(curLine);
  int lineEnd = (curLine + 1 < totalLines) ? editorGetLinePosition(curLine + 1) : (int)bufLen;
  int dispEnd = lineEnd;
  if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

  char lineBuf[256];
  int len = dispEnd - lineStart;
  int copyLen = len < 0 ? 0 : (len > (int)sizeof(lineBuf) - 1 ? (int)sizeof(lineBuf) - 1 : len);
  strncpy(lineBuf, buf + lineStart, copyLen);
  lineBuf[copyLen] = '\0';

  int prefixLen = (curCol < copyLen) ? curCol : copyLen;
  int headingLevel = mdHeadingLevel(lineBuf, copyLen);
  uint8_t baseStyle = headingLevel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

  int cursorX = textX + measureStyledPrefix(renderer, lineBuf, copyLen, prefixLen, baseStyle);
  int cursorW = renderer.getSpaceWidth(g_bodyFont);
  if (cursorW < 2) cursorW = 8;

  if (cursorX >= 0 && cursorX + cursorW <= textRight && cursorY >= 0 &&
      cursorY + lineHeight <= renderer.getScreenHeight()) {
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, true);
  }
}

// Slim header: title (bold) + unsaved marker, word count, keyboard-drop marker,
// divider. Returns the top y of the text area. Battery/mode/dark-mode chrome
// from MicroSlate is intentionally dropped for now.
int drawHeader(GfxRenderer& renderer, int sw, int insetTop, int insetRight, int insetLeft) {
  const char* title = editorGetCurrentTitle();
  char headerBuf[64];
  if (editorHasUnsavedChanges()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s *", title);
  } else {
    strncpy(headerBuf, title, sizeof(headerBuf) - 1);
    headerBuf[sizeof(headerBuf) - 1] = '\0';
  }

  const int textY = 5 + insetTop;
  int rightAnchor = sw - kMargin - insetRight;
  if (!BleHid.isConnected()) {
    const char* kbInd = "[no kb]";
    int kbW = renderer.getTextAdvanceX(kUiFont, kbInd, EpdFontFamily::REGULAR);
    int kbX = rightAnchor - kbW;
    renderer.drawText(kUiFont, kbX, textY, kbInd, true, EpdFontFamily::REGULAR);
    rightAnchor = kbX - 8;
  }

  int wc = editorGetWordCount();
  char wcBuf[24];
  if (wc == 1)
    snprintf(wcBuf, sizeof(wcBuf), "1 word");
  else
    snprintf(wcBuf, sizeof(wcBuf), "%d words", wc);
  int wcW = renderer.getTextAdvanceX(kUiFont, wcBuf, EpdFontFamily::REGULAR);
  int wcX = rightAnchor - wcW;
  renderer.drawText(kUiFont, wcX, textY, wcBuf, true, EpdFontFamily::REGULAR);

  renderer.drawText(kUiFont, kMargin + insetLeft, textY, headerBuf, true, EpdFontFamily::BOLD);

  renderer.drawLine(5 + insetLeft, 32 + insetTop, sw - 5 - insetRight, 32 + insetTop, true);
  return 38 + insetTop;
}
// Strip the directory portion of a path, leaving just the file name.
const char* baseName(const std::string& path) {
  const char* slash = strrchr(path.c_str(), '/');
  return slash ? slash + 1 : path.c_str();
}
}  // namespace

void EditorActivity::loadNote() {
  if (filePath_.empty() || !Storage.exists(filePath_.c_str())) return;

  // Read straight into the editor's own buffer — no intermediate copy, which
  // matters on this tight heap.
  char* buf = editorGetBuffer();
  size_t n = Storage.readFileToBuffer(filePath_.c_str(), buf, TEXT_BUFFER_SIZE);
  editorLoadBuffer(n);

  const char* base = baseName(filePath_);
  char title[MAX_TITLE_LEN];
  filenameToTitle(base, title, MAX_TITLE_LEN);
  editorSetCurrentTitle(title);
  editorSetCurrentFile(base);
  editorSetUnsavedChanges(false);
  LOG_INF("EDITOR", "Loaded %s (%u bytes)", filePath_.c_str(), (unsigned)n);
}

bool EditorActivity::saveNote() {
  Storage.ensureDirectoryExists(kNotesDir);

  // New note: derive a filename from the title, uniquified against /notes.
  if (filePath_.empty()) {
    char fn[MAX_FILENAME_LEN];
    titleToFilename(editorGetCurrentTitle(), fn, MAX_FILENAME_LEN);

    std::string candidate = std::string(kNotesDir) + "/" + fn;
    if (Storage.exists(candidate.c_str())) {
      // Insert " N" before the ".md" until a free name is found.
      std::string stem(fn);
      std::string ext;
      size_t dot = stem.rfind('.');
      if (dot != std::string::npos) {
        ext = stem.substr(dot);
        stem = stem.substr(0, dot);
      }
      for (int i = 2; i < 1000; i++) {
        candidate = std::string(kNotesDir) + "/" + stem + "_" + std::to_string(i) + ext;
        if (!Storage.exists(candidate.c_str())) break;
      }
    }
    filePath_ = candidate;
    editorSetCurrentFile(baseName(filePath_));
  }

  // Write via an FsFile so the 16KB buffer is streamed out, not copied into an
  // Arduino String (which would transiently double heap use).
  FsFile f;
  if (!Storage.openFileForWrite("EDITOR", filePath_.c_str(), f)) {
    LOG_ERR("EDITOR", "Save failed to open %s", filePath_.c_str());
    return false;
  }
  f.write(reinterpret_cast<const uint8_t*>(editorGetBuffer()), editorGetLength());
  f.close();
  editorSetUnsavedChanges(false);
  justSaved_ = true;
  LOG_INF("EDITOR", "Saved %s (%u bytes)", filePath_.c_str(), (unsigned)editorGetLength());
  return true;
}

void EditorActivity::onExit() {
  if (editorHasUnsavedChanges() && editorGetLength() > 0) saveNote();
  BleHid.end();                                // tear down NimBLE so its heap returns to the reader/home
  editorFree();                                // return the 20KB text/line buffers to the heap
  renderer.setOrientation(savedOrientation_);  // rotation is editor-local
  Activity::onExit();
}

void EditorActivity::recomputeCharsPerLine() {
  // Derive characters-per-line from the body font's advance so the core's
  // char-count wrap matches how many glyphs fit across the (current) screen.
  int sw = renderer.getScreenWidth();
  int glyphW = renderer.getTextWidth(g_bodyFont, "m");
  if (glyphW < 1) glyphW = 8;
  int insetTop, insetRight, insetBottom, insetLeft;
  legendInsets(renderer, legendStrip(), insetTop, insetRight, insetBottom, insetLeft);
  int cpl = (sw - 2 * kMargin - insetLeft - insetRight) / glyphW;
  if (cpl < 8) cpl = 8;
  editorSetCharsPerLine(cpl);
}

void EditorActivity::applyFontIndex() {
  int f = g_bodyFontSizes[fontIdx_];
  if (renderer.getLineHeight(f) <= 0) f = UI_12_FONT_ID;  // fallback if size not compiled in
  g_bodyFont = f;
  recomputeCharsPerLine();
  requestUpdate();
}

void EditorActivity::cycleOrientation() {
  using O = GfxRenderer::Orientation;
  O next = static_cast<O>((static_cast<int>(renderer.getOrientation()) + 1) % 4);
  renderer.setOrientation(next);
  recomputeCharsPerLine();  // width changed
  requestUpdate();
}

void EditorActivity::onEnter() {
  Activity::onEnter();

  if (!editorInit()) {
    LOG_ERR("EDITOR", "Failed to allocate editor buffers (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    initFailed_ = true;
    return;  // loop() sends us home on the next tick
  }

  savedOrientation_ = renderer.getOrientation();
  applyFontIndex();  // set g_bodyFont from fontIdx_ and derive the wrap width

  loadNote();  // no-op for a new (empty path) note

  // Free the SD reading-font caches before bringing up NimBLE. The editor renders
  // with the built-in UI fonts only, and on the X4 NimBLEDevice::init() needs a
  // large contiguous allocation that a full Home boot (which loads the SD font)
  // otherwise fragments away — causing intermittent "BleHid.begin() failed". The
  // reader reloads the SD font on demand (ReaderActivity::onEnter ensureLoaded).
  sdFontSystem.releaseForNetwork(renderer);
  LOG_INF("EDITOR", "Heap before BLE init: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (!BleHid.begin("CrossInk")) {
    LOG_ERR("EDITOR", "BleHid.begin() failed — capability compiled out or BLE init error");
  } else if (BleHid.pairedCount() > 0) {
    LOG_INF("EDITOR", "BLE up; %u bond(s) known, waiting for auto-reconnect", BleHid.pairedCount());
  } else {
    LOG_INF("EDITOR", "BLE up; no bonds, scanning for a keyboard");
    BleHid.startScan(8000);
    scanStartedAt_ = millis();
  }
  requestUpdate();
}

void EditorActivity::drainKeyboard() {
  bool dirty = false;
  KeyEvent ev;
  while (BleHid.popKey(ev)) {
    // Ctrl+S or Cmd+S saves (HID mods: ctrl 0x01/0x10, GUI/Cmd 0x08/0x80; 's' keycode 0x16).
    if ((ev.mods & 0x99) && ev.keycode == 0x16) {
      saveNote();
      dirty = true;
      continue;
    }
    switch (ev.special) {
      case SpecialKey::Enter:
        editorInsertChar('\n');
        dirty = true;
        continue;
      case SpecialKey::Backspace:
        editorDeleteChar();
        dirty = true;
        continue;
      case SpecialKey::Delete:
        editorDeleteForward();
        dirty = true;
        continue;
      case SpecialKey::Tab:
        editorInsertChar('\t');
        dirty = true;
        continue;
      case SpecialKey::Left:
        editorMoveCursorLeft();
        dirty = true;
        continue;
      case SpecialKey::Right:
        editorMoveCursorRight();
        dirty = true;
        continue;
      case SpecialKey::Up:
        editorMoveCursorUp();
        dirty = true;
        continue;
      case SpecialKey::Down:
        editorMoveCursorDown();
        dirty = true;
        continue;
      case SpecialKey::Home:
        editorMoveCursorHome();
        dirty = true;
        continue;
      case SpecialKey::End:
        editorMoveCursorEnd();
        dirty = true;
        continue;
      default:
        break;  // None / Escape / PageUp / PageDown — not handled yet
    }
    if (ev.mods & 0x11) continue;  // swallow other Ctrl+key combos (not typed text)
    if (ev.ch >= 32 && ev.ch < 127) {
      editorInsertChar(ev.ch);
      dirty = true;
    }
  }
  if (dirty) requestUpdate();
}

void EditorActivity::loop() {
  if (initFailed_) {
    onGoHome();
    return;
  }

  BleHid.poll();

  // On-device physical controls (the four bottom buttons, matching the legend):
  // Back = save & exit to Home; Confirm/Enter = rotate screen; Left = font
  // smaller; Right = font bigger.
  using Btn = MappedInputManager::Button;
  if (mappedInput.wasReleased(Btn::Back)) {
    if (editorHasUnsavedChanges() && editorGetLength() > 0) saveNote();
    onGoHome();  // triggers onExit(): save, BleHid.end(), restore orientation
    return;
  }
  if (mappedInput.wasReleased(Btn::Confirm)) {
    cycleOrientation();
    return;
  }
  if (mappedInput.wasReleased(Btn::Right)) {  // font bigger
    if (fontIdx_ + 1 < g_bodyFontCount) {
      fontIdx_++;
      applyFontIndex();
    }
    return;
  }
  if (mappedInput.wasReleased(Btn::Left)) {  // font smaller
    if (fontIdx_ > 0) {
      fontIdx_--;
      applyFontIndex();
    }
    return;
  }

  // One connect attempt at the first HID device a scan turns up (spike logic).
  if (!connectAttempted_ && !BleHid.isConnected() && !BleHid.isConnecting() && !BleHid.isScanning() &&
      BleHid.deviceCount() > 0) {
    for (uint8_t i = 0; i < BleHid.deviceCount(); i++) {
      const auto& d = BleHid.device(i);
      if (d.hid && d.connectable) {
        LOG_INF("EDITOR", "Connecting to %s (%s)", d.name, d.addr);
        BleHid.connect(d.addr);
        connectAttempted_ = true;
        requestUpdate();
        break;
      }
    }
    if (!connectAttempted_) {
      BleHid.startScan(8000);
      scanStartedAt_ = millis();
    }
  }

  if (BleHid.isConnected() != lastConnected_) {
    lastConnected_ = BleHid.isConnected();
    LOG_INF("EDITOR", "Link %s%s", lastConnected_ ? "UP: " : "DOWN", lastConnected_ ? BleHid.connectedName() : "");
    requestUpdate();
  }

  drainKeyboard();
}

void EditorActivity::render(RenderLock&&) {
  if (initFailed_) return;  // buffers absent; loop() is about to bounce home

  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  // Reserve the legend strip on whichever logical edge the physical bottom
  // (button row) currently maps to, so rotated layouts don't draw under it.
  int insetTop, insetRight, insetBottom, insetLeft;
  legendInsets(renderer, legendStrip(), insetTop, insetRight, insetBottom, insetLeft);

  int textAreaTop = drawHeader(renderer, sw, insetTop, insetRight, insetLeft);

  int lineHeight = renderer.getLineHeight(g_bodyFont);
  if (lineHeight <= 0) lineHeight = 20;

  int textAreaBottom = sh - insetBottom;
  int visibleLines = (textAreaBottom - textAreaTop) / lineHeight;
  if (visibleLines < 1) visibleLines = 1;
  editorSetVisibleLines(visibleLines);

  int vpStart = editorGetViewportStart();
  int totalLines = editorGetLineCount();
  int curLine = editorGetCursorLine();

  const int textX = kMargin + insetLeft;
  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int yPos = textAreaTop + (i * lineHeight);
    drawEditorLine(renderer, vpStart + i, textX, yPos);
  }

  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorY = textAreaTop + ((curLine - vpStart) * lineHeight);
    drawEditorCursor(renderer, cursorY, lineHeight, textX, sw - insetRight);
  }

  // Button legend (4 bottom buttons): Back=Exit, Confirm=Rotate, Left=A- (font
  // smaller), Right=A+ (font bigger). mapLabels order is (back, confirm, prev, next).
  const auto labels = mappedInput.mapLabels("Exit", "Rotate", "A-", "A+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif  // EDITOR_BLE_SPIKE
