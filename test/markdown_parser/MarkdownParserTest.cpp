// Host-side tests for the styled-source markdown parser (src/editor/markdown.h).
// Pure functions — no hardware, no Arduino. Ported from the MicroSlate unity
// suite; the parser moved verbatim.
#include <gtest/gtest.h>

#include <cstring>

#include "markdown.h"

namespace {

MdRun runs[24];

int parse(const char* s, uint8_t base = EpdFontFamily::REGULAR, int maxRuns = 24) {
  return mdParseInline(s, (int)strlen(s), base, runs, maxRuns);
}

void assertRun(int idx, int start, int len, uint8_t style) {
  EXPECT_EQ(start, runs[idx].start) << "run start";
  EXPECT_EQ(len, runs[idx].len) << "run len";
  EXPECT_EQ(style, (uint8_t)runs[idx].style) << "run style";
}

}  // namespace

// --- mdHeadingLevel ---

TEST(MarkdownHeadingLevel, DetectsH1H2H3) {
  EXPECT_EQ(1, mdHeadingLevel("# Title", 7));
  EXPECT_EQ(2, mdHeadingLevel("## Title", 8));
  EXPECT_EQ(3, mdHeadingLevel("### Title", 9));
}

TEST(MarkdownHeadingLevel, RequiresSpaceAfterHashes) {
  EXPECT_EQ(0, mdHeadingLevel("#Title", 6));
  EXPECT_EQ(0, mdHeadingLevel("##", 2));
  EXPECT_EQ(0, mdHeadingLevel("#", 1));
}

TEST(MarkdownHeadingLevel, CapsAtThree) {
  // "#### x": four hashes — h stops at 3, line[3] is '#', not ' ' -> 0
  EXPECT_EQ(0, mdHeadingLevel("#### x", 6));
}

TEST(MarkdownHeadingLevel, IgnoresMidLineAndIndentedHashes) {
  EXPECT_EQ(0, mdHeadingLevel(" # Title", 8));
  EXPECT_EQ(0, mdHeadingLevel("a # b", 5));
  EXPECT_EQ(0, mdHeadingLevel("", 0));
}

TEST(MarkdownHeadingLevel, HashSpaceOnly) {
  // "# " is a valid (empty) heading per the parser: hash then space
  EXPECT_EQ(1, mdHeadingLevel("# ", 2));
}

// --- mdParseInline ---

TEST(MarkdownParseInline, PlainTextIsSingleRegularRun) {
  EXPECT_EQ(1, parse("hello world"));
  assertRun(0, 0, 11, EpdFontFamily::REGULAR);
}

TEST(MarkdownParseInline, EmptyLineYieldsNoRuns) {
  EXPECT_EQ(0, parse(""));
}

TEST(MarkdownParseInline, BoldSpanStylesMarkersWithSpan) {
  // "a **b** c" -> "a " regular, "**" bold (union), "b" bold, "**" bold, " c" regular
  EXPECT_EQ(5, parse("a **b** c"));
  assertRun(0, 0, 2, EpdFontFamily::REGULAR);
  assertRun(1, 2, 2, EpdFontFamily::BOLD);
  assertRun(2, 4, 1, EpdFontFamily::BOLD);
  assertRun(3, 5, 2, EpdFontFamily::BOLD);
  assertRun(4, 7, 2, EpdFontFamily::REGULAR);
}

TEST(MarkdownParseInline, ItalicSpanTogglesOnSingleStar) {
  EXPECT_EQ(5, parse("a *b* c"));
  assertRun(0, 0, 2, EpdFontFamily::REGULAR);
  assertRun(1, 2, 1, EpdFontFamily::ITALIC);
  assertRun(2, 3, 1, EpdFontFamily::ITALIC);
  assertRun(3, 4, 1, EpdFontFamily::ITALIC);
  assertRun(4, 5, 2, EpdFontFamily::REGULAR);
}

TEST(MarkdownParseInline, TripleStarTogglesBoldItalic) {
  EXPECT_EQ(3, parse("***x***"));
  assertRun(0, 0, 3, EpdFontFamily::BOLD_ITALIC);
  assertRun(1, 3, 1, EpdFontFamily::BOLD_ITALIC);
  assertRun(2, 4, 3, EpdFontFamily::BOLD_ITALIC);
}

TEST(MarkdownParseInline, UnclosedMarkerStylesToEndOfLine) {
  EXPECT_EQ(3, parse("a **rest"));
  assertRun(0, 0, 2, EpdFontFamily::REGULAR);
  assertRun(1, 2, 2, EpdFontFamily::BOLD);  // unclosed "**" marker
  assertRun(2, 4, 4, EpdFontFamily::BOLD);  // "rest" styled to EOL
}

TEST(MarkdownParseInline, NestedItalicInsideBold) {
  // "**a *b* c**": bold throughout, italic toggled inside
  EXPECT_EQ(7, parse("**a *b* c**"));
  assertRun(0, 0, 2, EpdFontFamily::BOLD);         // opening **
  assertRun(1, 2, 2, EpdFontFamily::BOLD);         // "a "
  assertRun(2, 4, 1, EpdFontFamily::BOLD_ITALIC);  // opening *
  assertRun(3, 5, 1, EpdFontFamily::BOLD_ITALIC);  // "b"
  assertRun(4, 6, 1, EpdFontFamily::BOLD_ITALIC);  // closing *
  assertRun(5, 7, 2, EpdFontFamily::BOLD);         // " c"
  assertRun(6, 9, 2, EpdFontFamily::BOLD);         // closing **
}

TEST(MarkdownParseInline, BaseStyleSeedsHeadingBold) {
  // Heading line body: base BOLD, italic span becomes BOLD_ITALIC
  EXPECT_EQ(4, parse("x *y*", EpdFontFamily::BOLD));
  assertRun(0, 0, 2, EpdFontFamily::BOLD);
  assertRun(1, 2, 1, EpdFontFamily::BOLD_ITALIC);
  assertRun(2, 3, 1, EpdFontFamily::BOLD_ITALIC);
  assertRun(3, 4, 1, EpdFontFamily::BOLD_ITALIC);
}

TEST(MarkdownParseInline, MarkerOnlyLine) {
  // "**" alone: single bold run (marker styled with the span it opens)
  EXPECT_EQ(1, parse("**"));
  assertRun(0, 0, 2, EpdFontFamily::BOLD);
}

TEST(MarkdownParseInline, FourStarsToggleBoldTwice) {
  // "****" = "***" (bold+italic toggle) then "*" (italic toggle): parser
  // consumes max 3 stars per marker. Net: two markers, both rendered.
  int n = parse("****x");
  EXPECT_EQ(3, n);
  // First marker "***" toggles REGULAR->BOLD_ITALIC, union = BOLD_ITALIC
  assertRun(0, 0, 3, EpdFontFamily::BOLD_ITALIC);
  // Second marker "*" toggles BOLD_ITALIC->BOLD, union = BOLD_ITALIC
  assertRun(1, 3, 1, EpdFontFamily::BOLD_ITALIC);
  // "x" bold
  assertRun(2, 4, 1, EpdFontFamily::BOLD);
}

TEST(MarkdownParseInline, MaxRunsOverflowDropsExcessRunsSafely) {
  // 10 italic spans -> far more than 4 runs; parser must not write past maxRuns
  const char* line = "*a* *b* *c* *d* *e*";
  int n = mdParseInline(line, (int)strlen(line), EpdFontFamily::REGULAR, runs, 4);
  EXPECT_EQ(4, n);
}
