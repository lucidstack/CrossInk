// Host-side tests for the editor buffer (src/editor/text_editor.cpp) — word
// wrap, insert/delete shifting, cursor line/column math, word count. The editor
// translation unit is pure C++ (no Arduino), so it compiles natively. Ported
// from the MicroSlate unity suite.
#include <gtest/gtest.h>

#include <cstring>

#include "text_editor.h"

namespace {

void typeString(const char* s) {
  for (const char* p = s; *p; p++) editorInsertChar(*p);
}

// Fixture resets the editor's file-static state before every case, mirroring the
// unity setUp(): fresh buffer, small width for easy wrap cases.
class TextEditor : public ::testing::Test {
 protected:
  void SetUp() override {
    editorInit();
    editorSetCharsPerLine(10);  // small width -> easy wrap cases
    editorSetVisibleLines(5);
  }
};

}  // namespace

// --- Insert / delete ---

TEST_F(TextEditor, InsertTypesAtCursorAndAdvances) {
  typeString("abc");
  EXPECT_STREQ("abc", editorGetBuffer());
  EXPECT_EQ(3, editorGetCursorPosition());
  EXPECT_TRUE(editorHasUnsavedChanges());
}

TEST_F(TextEditor, InsertMidBufferShiftsTail) {
  typeString("ac");
  editorMoveCursorLeft();
  editorInsertChar('b');
  EXPECT_STREQ("abc", editorGetBuffer());
  EXPECT_EQ(2, editorGetCursorPosition());
}

TEST_F(TextEditor, BackspaceRemovesBeforeCursor) {
  typeString("abc");
  editorDeleteChar();
  EXPECT_STREQ("ab", editorGetBuffer());
  EXPECT_EQ(2, editorGetCursorPosition());
}

TEST_F(TextEditor, BackspaceAtStartIsNoop) {
  typeString("a");
  editorMoveCursorLeft();
  editorDeleteChar();
  EXPECT_STREQ("a", editorGetBuffer());
  EXPECT_EQ(0, editorGetCursorPosition());
}

TEST_F(TextEditor, DeleteForwardRemovesAtCursor) {
  typeString("abc");
  editorMoveCursorHome();
  editorDeleteForward();
  EXPECT_STREQ("bc", editorGetBuffer());
  EXPECT_EQ(0, editorGetCursorPosition());
}

TEST_F(TextEditor, DeleteForwardAtEndIsNoop) {
  typeString("ab");
  editorDeleteForward();
  EXPECT_STREQ("ab", editorGetBuffer());
}

// --- Word wrap (charsPerLine = 10) ---

TEST_F(TextEditor, ShortLineDoesNotWrap) {
  typeString("hello");
  EXPECT_EQ(1, editorGetLineCount());
}

TEST_F(TextEditor, WrapBreaksAfterLastSpace) {
  // "aaaa bbbbcc" hits col 10 at the second 'c'; last space is index 4
  // -> line 2 starts at index 5
  typeString("aaaa bbbbcc");
  EXPECT_EQ(2, editorGetLineCount());
  EXPECT_EQ(5, editorGetLinePosition(1));
}

TEST_F(TextEditor, WrapBreaksMidWordWithoutSpace) {
  typeString("abcdefghijkl");  // 12 chars, no space
  EXPECT_EQ(2, editorGetLineCount());
  EXPECT_EQ(10, editorGetLinePosition(1));
}

TEST_F(TextEditor, HardNewlineBreaksLine) {
  typeString("ab\ncd");
  EXPECT_EQ(2, editorGetLineCount());
  EXPECT_EQ(3, editorGetLinePosition(1));  // after '\n'
}

// --- Cursor line/column ---

TEST_F(TextEditor, CursorColumnTracksWrap) {
  typeString("abcdefghijkl");  // wraps at 10
  EXPECT_EQ(1, editorGetCursorLine());
  EXPECT_EQ(2, editorGetCursorCol());  // "kl" -> col 2 (after 'l')
}

TEST_F(TextEditor, CursorUpClampsColumnToShorterLine) {
  typeString("ab\nlonger");
  // cursor at end of "longer" (col 6); move up to "ab" (len 2 + newline)
  editorMoveCursorUp();
  EXPECT_EQ(0, editorGetCursorLine());
  EXPECT_TRUE(editorGetCursorCol() <= 2);
}

TEST_F(TextEditor, CursorDownFromFirstLine) {
  typeString("ab\ncd");
  editorMoveCursorHome();
  editorMoveCursorUp();  // already line 0 -> noop
  EXPECT_EQ(0, editorGetCursorLine());
  editorMoveCursorDown();
  EXPECT_EQ(1, editorGetCursorLine());
}

TEST_F(TextEditor, HomeEndMoveWithinLine) {
  typeString("hello");
  editorMoveCursorHome();
  EXPECT_EQ(0, editorGetCursorPosition());
  editorMoveCursorEnd();
  EXPECT_EQ(5, editorGetCursorPosition());
}

// --- Word count ---

TEST_F(TextEditor, WordCountBasics) {
  EXPECT_EQ(0, editorGetWordCount());
  typeString("one two  three\nfour\tfive");
  EXPECT_EQ(5, editorGetWordCount());
}

// --- Buffer capacity ---

TEST_F(TextEditor, BufferFullDropsInsertSilently) {
  // Fill to capacity via editorLoadBuffer, then verify insert is a no-op
  char* buf = editorGetBuffer();
  memset(buf, 'x', TEXT_BUFFER_SIZE - 1);
  editorLoadBuffer(TEXT_BUFFER_SIZE - 1);
  size_t before = editorGetLength();
  editorInsertChar('y');
  EXPECT_EQ(before, editorGetLength());
}
