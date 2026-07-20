#include "EditorActivity.h"

#include <BleKeyboardHost.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "editor/markdown.h"
#include "editor/note_names.h"
#include "editor/text_editor.h"
#include "fontIds.h"

using freeink::KeyEvent;
using freeink::SpecialKey;

namespace {
constexpr int kBodyFont = UI_12_FONT_ID;
constexpr int kUiFont = UI_10_FONT_ID;
constexpr int kMargin = 10;

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
    renderer.drawText(kBodyFont, xCur, yPos, runBuf, true, runs[r].style);
    xCur += renderer.getTextWidth(kBodyFont, runBuf, runs[r].style);
  }

  // H1 gets an underline for hierarchy without breaking the uniform line height.
  if (headingLevel == 1) {
    int underlineY = yPos + renderer.getFontAscenderSize(kBodyFont) + 3;
    if (xCur > x) renderer.drawLine(x, underlineY, xCur, underlineY, true);
  }
}

// Pixel width of the first prefixLen bytes of a line using the same
// heading/inline styling as drawEditorLine, so the caret lands where the styled
// glyphs actually end. Runs are parsed over the FULL line (a prefix-only parse
// would mis-style text after an opening marker that closes past the cursor),
// then only the prefix portion is measured.
int measureStyledPrefix(GfxRenderer& renderer, const char* lineBuf, int lineLen, int prefixLen,
                        uint8_t baseStyle) {
  MdRun runs[24];
  int runCount = mdParseInline(lineBuf, lineLen, baseStyle, runs, 24);

  int w = 0;
  char runBuf[256];
  for (int r = 0; r < runCount && runs[r].start < prefixLen; r++) {
    int take = runs[r].len;
    if (runs[r].start + take > prefixLen) take = prefixLen - runs[r].start;
    memcpy(runBuf, lineBuf + runs[r].start, take);
    runBuf[take] = '\0';
    w += renderer.getTextWidth(kBodyFont, runBuf, runs[r].style);
  }
  return w;
}

void drawEditorCursor(GfxRenderer& renderer, int cursorY, int lineHeight, int sw) {
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

  int cursorX = kMargin + measureStyledPrefix(renderer, lineBuf, copyLen, prefixLen, baseStyle);
  int cursorW = renderer.getSpaceWidth(kBodyFont);
  if (cursorW < 2) cursorW = 8;

  if (cursorX >= 0 && cursorX + cursorW <= sw && cursorY >= 0 &&
      cursorY + lineHeight <= renderer.getScreenHeight()) {
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, true);
  }
}

// Slim header: title (bold) + unsaved marker, word count, keyboard-drop marker,
// divider. Returns the top y of the text area. Battery/mode/dark-mode chrome
// from MicroSlate is intentionally dropped for now.
int drawHeader(GfxRenderer& renderer, int sw) {
  const char* title = editorGetCurrentTitle();
  char headerBuf[64];
  if (editorHasUnsavedChanges()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s *", title);
  } else {
    strncpy(headerBuf, title, sizeof(headerBuf) - 1);
    headerBuf[sizeof(headerBuf) - 1] = '\0';
  }

  int rightAnchor = sw - kMargin;
  if (!BleHid.isConnected()) {
    const char* kbInd = "[no kb]";
    int kbW = renderer.getTextAdvanceX(kUiFont, kbInd, EpdFontFamily::REGULAR);
    int kbX = rightAnchor - kbW;
    renderer.drawText(kUiFont, kbX, 5, kbInd, true, EpdFontFamily::REGULAR);
    rightAnchor = kbX - 8;
  }

  int wc = editorGetWordCount();
  char wcBuf[24];
  if (wc == 1) snprintf(wcBuf, sizeof(wcBuf), "1 word");
  else snprintf(wcBuf, sizeof(wcBuf), "%d words", wc);
  int wcW = renderer.getTextAdvanceX(kUiFont, wcBuf, EpdFontFamily::REGULAR);
  int wcX = rightAnchor - wcW;
  renderer.drawText(kUiFont, wcX, 5, wcBuf, true, EpdFontFamily::REGULAR);

  renderer.drawText(kUiFont, kMargin, 5, headerBuf, true, EpdFontFamily::BOLD);

  renderer.drawLine(5, 32, sw - 5, 32, true);
  return 38;
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
  Activity::onExit();
}

void EditorActivity::onEnter() {
  Activity::onEnter();

  editorInit();

  // Derive characters-per-line from the body font's advance so the core's
  // char-count wrap matches how many glyphs fit across the screen.
  int sw = renderer.getScreenWidth();
  int glyphW = renderer.getTextWidth(kBodyFont, "m");
  if (glyphW < 1) glyphW = 8;
  int cpl = (sw - 2 * kMargin) / glyphW;
  if (cpl < 8) cpl = 8;
  editorSetCharsPerLine(cpl);

  loadNote();  // no-op for a new (empty path) note

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
    // Ctrl+S saves (HID: left ctrl 0x01 / right ctrl 0x10, 's' keycode 0x16).
    if ((ev.mods & 0x11) && ev.keycode == 0x16) {
      saveNote();
      dirty = true;
      continue;
    }
    switch (ev.special) {
      case SpecialKey::Enter:     editorInsertChar('\n');   dirty = true; continue;
      case SpecialKey::Backspace: editorDeleteChar();       dirty = true; continue;
      case SpecialKey::Delete:    editorDeleteForward();    dirty = true; continue;
      case SpecialKey::Tab:       editorInsertChar('\t');   dirty = true; continue;
      case SpecialKey::Left:      editorMoveCursorLeft();   dirty = true; continue;
      case SpecialKey::Right:     editorMoveCursorRight();  dirty = true; continue;
      case SpecialKey::Up:        editorMoveCursorUp();     dirty = true; continue;
      case SpecialKey::Down:      editorMoveCursorDown();   dirty = true; continue;
      case SpecialKey::Home:      editorMoveCursorHome();   dirty = true; continue;
      case SpecialKey::End:       editorMoveCursorEnd();    dirty = true; continue;
      default: break;  // None / Escape / PageUp / PageDown — not handled yet
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
  BleHid.poll();

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
  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  int textAreaTop = drawHeader(renderer, sw);

  int lineHeight = renderer.getLineHeight(kBodyFont);
  if (lineHeight <= 0) lineHeight = 20;

  int textAreaBottom = sh - 5;
  int visibleLines = (textAreaBottom - textAreaTop) / lineHeight;
  if (visibleLines < 1) visibleLines = 1;
  editorSetVisibleLines(visibleLines);

  int vpStart = editorGetViewportStart();
  int totalLines = editorGetLineCount();
  int curLine = editorGetCursorLine();

  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int yPos = textAreaTop + (i * lineHeight);
    drawEditorLine(renderer, vpStart + i, kMargin, yPos);
  }

  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorY = textAreaTop + ((curLine - vpStart) * lineHeight);
    drawEditorCursor(renderer, cursorY, lineHeight, sw);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
