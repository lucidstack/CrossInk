#pragma once
#include <EpdFontFamily.h>

// Markdown support for the styled-source editor: markers stay visible and the
// text between them is styled live. Parsing is line-scoped and happens at
// render time for the visible lines only — a line parses in microseconds, so
// nothing is cached and the flat text buffer stays plain text on disk.

struct MdRun {
  int start;  // byte offset in the line
  int len;
  EpdFontFamily::Style style;
};

// Heading level 1-3 for lines starting "# ", "## ", "### "; 0 otherwise.
inline int mdHeadingLevel(const char* line, int len) {
  int h = 0;
  while (h < 3 && h < len && line[h] == '#') h++;
  return (h > 0 && h < len && line[h] == ' ') ? h : 0;
}

// Split a line into styled runs honoring **bold** and *italic* toggles.
// Marker characters are kept in the output (styled like the region they
// delimit) so the source text stays fully visible and cursor/wrap math is
// unchanged. An unclosed marker styles the rest of the line — which is what
// you want while typing. baseStyle seeds the line style (BOLD for headings).
// Style bits align with EpdFontFamily: BOLD=1, ITALIC=2, BOLD_ITALIC=3.
inline int mdParseInline(const char* s, int len, uint8_t baseStyle, MdRun* runs, int maxRuns) {
  uint8_t style = baseStyle;
  int runStart = 0;
  int n = 0;

  auto flush = [&](int end, uint8_t st) {
    if (end > runStart && n < maxRuns) {
      runs[n].start = runStart;
      runs[n].len = end - runStart;
      runs[n].style = static_cast<EpdFontFamily::Style>(st);
      n++;
    }
    runStart = end;
  };

  int i = 0;
  while (i < len) {
    if (s[i] == '*') {
      int stars = 1;
      while (i + stars < len && s[i + stars] == '*' && stars < 3) stars++;

      uint8_t newStyle = style;
      if (stars >= 2) newStyle ^= EpdFontFamily::BOLD;
      if (stars == 1 || stars == 3) newStyle ^= EpdFontFamily::ITALIC;

      flush(i, style);
      // Draw the marker itself with the union of both sides' styles so
      // opening and closing markers visually attach to their span.
      flush(i + stars, style | newStyle);
      style = newStyle;
      i += stars;
    } else {
      i++;
    }
  }
  flush(len, style);
  return n;
}
