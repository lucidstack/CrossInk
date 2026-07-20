// Host-side tests for filename <-> title conversion (src/editor/note_names.h).
// Ported from the MicroSlate unity suite.
#include <gtest/gtest.h>

#include <cstring>

#include "note_names.h"

namespace {

class NoteNames : public ::testing::Test {
 protected:
  void SetUp() override { memset(out, 0, sizeof(out)); }
  char out[64];
};

}  // namespace

// --- filenameToTitle ---

TEST_F(NoteNames, TitleCapitalizesWordsAndDropsExtension) {
  filenameToTitle("my_note_2.txt", out, sizeof(out));
  EXPECT_STREQ("My Note 2", out);
}

TEST_F(NoteNames, TitleHandlesMdExtension) {
  filenameToTitle("shopping_list.md", out, sizeof(out));
  EXPECT_STREQ("Shopping List", out);
}

TEST_F(NoteNames, TitleEmptyFilenameFallsBackToUntitled) {
  filenameToTitle("", out, sizeof(out));
  EXPECT_STREQ("Untitled", out);
}

TEST_F(NoteNames, TitleExtensionOnlyFallsBackToUntitled) {
  filenameToTitle(".md", out, sizeof(out));
  EXPECT_STREQ("Untitled", out);
}

TEST_F(NoteNames, TitleLeadingUnderscoreDoesNotEmitSpace) {
  filenameToTitle("_note.md", out, sizeof(out));
  EXPECT_STREQ("Note", out);
}

// --- titleToFilename ---

TEST_F(NoteNames, FilenameLowercasesAndUnderscores) {
  titleToFilename("My Note 2", out, sizeof(out));
  EXPECT_STREQ("my_note_2.md", out);
}

TEST_F(NoteNames, FilenameStripsPunctuation) {
  titleToFilename("Hello, World!", out, sizeof(out));
  EXPECT_STREQ("hello_world.md", out);
}

TEST_F(NoteNames, FilenameCollapsesSeparatorRuns) {
  titleToFilename("a  -  b", out, sizeof(out));
  EXPECT_STREQ("a_b.md", out);
}

TEST_F(NoteNames, FilenameTrimsTrailingSeparators) {
  titleToFilename("note ", out, sizeof(out));
  EXPECT_STREQ("note.md", out);
}

TEST_F(NoteNames, FilenameAllPunctuationFallsBackToNote) {
  titleToFilename("!!!", out, sizeof(out));
  EXPECT_STREQ("note.md", out);
}

TEST_F(NoteNames, RoundTripPreservesSimpleTitles) {
  titleToFilename("Meeting Notes", out, sizeof(out));
  char back[64];
  filenameToTitle(out, back, sizeof(back));
  EXPECT_STREQ("Meeting Notes", back);
}
