// Host tests for the pure HID report-frame decoder (HidReportDecode.h).
// The frame sequences below are taken from real on-device serial captures of a
// "WirelessKeyboard 2" (report map: kbd=1 consumer=1 preferredByte=2) and encode
// the exact field bugs that were fixed, so they can never regress:
//   - modifier-only frames must emit nothing (Option/LGUI 0x08 -> 'e'),
//   - rollover trailing frames must emit nothing (doubled letters "teesttinng"),
//   - the generic fallback must not fire on genuine keyboard reports.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "HidReportDecode.h"

using freeink::DecodedKey;
using freeink::HidDecodeHints;
using freeink::HidReportDecoder;

namespace {

// The dual-page keyboard from the field captures.
HidDecodeHints keyboardHints() {
  HidDecodeHints h;
  h.hasKeyboardPage = true;
  h.hasConsumerPage = true;
  h.preferredByteIndex = 2;
  return h;
}

int feed(HidReportDecoder& d, const std::vector<uint8_t>& frame, std::vector<DecodedKey>& out) {
  return d.feed(frame.data(), frame.size(), out);
}

// Feed a sequence of raw frames; return the flat list of emitted usages.
std::vector<uint8_t> emittedUsages(HidReportDecoder& d, const std::vector<std::vector<uint8_t>>& frames) {
  std::vector<DecodedKey> out;
  for (const auto& f : frames) d.feed(f.data(), f.size(), out);
  std::vector<uint8_t> usages;
  for (const auto& k : out) usages.push_back(k.usage);
  return usages;
}

}  // namespace

// A plain key press emits once; its release emits nothing.
TEST(HidReportDecode, SingleKeyPressThenRelease) {
  HidReportDecoder d(keyboardHints());
  std::vector<DecodedKey> out;
  EXPECT_EQ(1, feed(d, {0, 0, 0x04, 0, 0, 0, 0, 0}, out));  // 'a'
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ(0x04, out[0].usage);
  EXPECT_EQ(0x00, out[0].mods);
  EXPECT_EQ(0, feed(d, {0, 0, 0, 0, 0, 0, 0, 0}, out));  // release
}

// Regression: a modifier-only frame (Option = LGUI 0x08) must emit NOTHING.
// Previously the generic consumer fallback grabbed the modifier byte as a usage,
// and 0x08 maps to 'e'. Shift-only (0x02) and RAlt (0x40) likewise emit nothing.
TEST(HidReportDecode, ModifierOnlyFrameEmitsNothing) {
  HidReportDecoder d(keyboardHints());
  EXPECT_TRUE(emittedUsages(d, {{0x08, 0, 0, 0, 0, 0, 0, 0}}).empty());  // LGUI/Option
  d.reset();
  EXPECT_TRUE(emittedUsages(d, {{0x02, 0, 0, 0, 0, 0, 0, 0}}).empty());  // LShift
  d.reset();
  EXPECT_TRUE(emittedUsages(d, {{0x40, 0, 0, 0, 0, 0, 0, 0}}).empty());  // RAlt
}

// Regression: rollover typing must not double the trailing key. Captured frames
// for "tes" where each next key is pressed before the previous is released.
TEST(HidReportDecode, RolloverDoesNotDouble) {
  HidReportDecoder d(keyboardHints());
  const std::vector<std::vector<uint8_t>> frames = {
      {0, 0, 0x17, 0, 0, 0, 0, 0},     // t down
      {0, 0, 0x17, 0x08, 0, 0, 0, 0},  // t held, e down
      {0, 0, 0x08, 0, 0, 0, 0, 0},     // e held (t released) -> NO new key
      {0, 0, 0x08, 0x16, 0, 0, 0, 0},  // e held, s down
      {0, 0, 0x16, 0, 0, 0, 0, 0},     // s held (e released) -> NO new key
      {0, 0, 0, 0, 0, 0, 0, 0},        // release
  };
  const std::vector<uint8_t> expected = {0x17, 0x08, 0x16};  // t, e, s — once each
  EXPECT_EQ(expected, emittedUsages(d, frames));
}

// Shift+letter: the letter usage is emitted and carries the shift modifier
// (translation to uppercase is HidKeymap's job; the decoder just forwards mods).
TEST(HidReportDecode, ShiftedLetterCarriesModifier) {
  HidReportDecoder d(keyboardHints());
  std::vector<DecodedKey> out;
  feed(d, {0x02, 0, 0x04, 0, 0, 0, 0, 0}, out);  // Shift + 'a'
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ(0x04, out[0].usage);
  EXPECT_EQ(0x02, out[0].mods);
}

// A 9-byte report with a leading report id is normalized before decoding.
TEST(HidReportDecode, StripsLeadingReportId) {
  HidReportDecoder d(keyboardHints());
  std::vector<DecodedKey> out;
  feed(d, {0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0}, out);  // id=1, then 'a'
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ(0x04, out[0].usage);
}

// Non-keyboard remote (consumer page only): the generic fallback reads the
// hinted byte, edge-detects one event per press, and re-triggers after release.
TEST(HidReportDecode, GenericFallbackForConsumerRemote) {
  HidDecodeHints h;
  h.hasKeyboardPage = false;
  h.hasConsumerPage = true;
  h.preferredByteIndex = 1;
  HidReportDecoder d(h);

  std::vector<DecodedKey> out;
  EXPECT_EQ(1, feed(d, {0x00, 0x29, 0x00}, out));  // button code 0x29
  EXPECT_EQ(0, feed(d, {0x00, 0x29, 0x00}, out));  // still held -> no repeat
  EXPECT_EQ(0, feed(d, {0x00, 0x00, 0x00}, out));  // release
  EXPECT_EQ(1, feed(d, {0x00, 0x29, 0x00}, out));  // pressed again -> re-emits
}

// extractPrimaryCode: with a report-map hint, trust it exclusively; a zero at the
// hinted byte means "no code" (never fall back to scanning the modifier byte).
TEST(HidReportDecode, ExtractPrimaryTrustsHint) {
  const HidDecodeHints h = keyboardHints();  // preferredByte = 2
  const std::vector<uint8_t> modifierOnly = {0x08, 0x00, 0x00, 0, 0, 0, 0, 0};
  EXPECT_EQ(0x00, HidReportDecoder::extractPrimaryCode(modifierOnly.data(), modifierOnly.size(), h));  // NOT 0x08
  const std::vector<uint8_t> withKey = {0x00, 0x00, 0x04, 0, 0, 0, 0, 0};
  EXPECT_EQ(0x04, HidReportDecoder::extractPrimaryCode(withKey.data(), withKey.size(), h));

  HidDecodeHints noHint;  // preferredByteIndex = 0xFF -> scan
  const std::vector<uint8_t> scan = {0x00, 0x00, 0x2C};
  EXPECT_EQ(0x2C, HidReportDecoder::extractPrimaryCode(scan.data(), scan.size(), noHint));
}
