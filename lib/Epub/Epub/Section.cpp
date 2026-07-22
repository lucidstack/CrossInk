#include "Section.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <ScratchWorkspace.h>
#include <Serialization.h>

#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
// v44: TextBlock word data is stored as one flat arena with optional bionic,
// guide-dot, and word-flag arrays.
constexpr uint8_t SECTION_FILE_VERSION = 44;
constexpr uint16_t INITIAL_SECTION_PAGE_LUT_ENTRIES = 1024;
constexpr uint32_t HEADER_SIZE = sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) +
                                 sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                 sizeof(bool) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t SECTION_HTML_STREAM_CHUNK_SIZE = 8192;
constexpr size_t LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE = 1024;

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};

template <typename Entry>
bool ensurePageLutCapacity(std::unique_ptr<Entry[]>& lut, uint16_t& lutCapacity, const uint16_t lutCount) {
  if (lutCount < lutCapacity) return true;
  if (lutCapacity == UINT16_MAX) return false;

  uint32_t nextCapacity = static_cast<uint32_t>(lutCapacity) * 2U;
  if (nextCapacity > UINT16_MAX) {
    nextCapacity = UINT16_MAX;
  }

  auto grown = makeUniqueNoThrow<Entry[]>(nextCapacity);
  if (!grown) return false;

  for (uint16_t i = 0; i < lutCount; i++) {
    grown[i] = lut[i];
  }
  lut = std::move(grown);
  lutCapacity = static_cast<uint16_t>(nextCapacity);
  return true;
}

ScratchWorkspace::Lease acquireSectionZipInflateScratch(GfxRenderer& renderer, const int fontId, const char* reason) {
  if (ESP.getMaxAllocHeap() < InflateReader::STREAMING_DICT_SIZE && renderer.isSdCardFont(fontId)) {
    renderer.releaseSdCardFontForLowMemory(fontId);
  }

  auto scratch = ScratchWorkspace::acquire(InflateReader::STREAMING_DICT_SIZE, reason);
  if (scratch || !renderer.isSdCardFont(fontId)) {
    return scratch;
  }

  renderer.releaseSdCardFontForLowMemory(fontId);
  return ScratchWorkspace::acquire(InflateReader::STREAMING_DICT_SIZE, reason);
}

size_t sectionHtmlStreamChunkSize(const bool preview) {
  if (preview) {
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }

  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  const size_t largeStreamBudget = InflateReader::STREAMING_DICT_SIZE + (2U * SECTION_HTML_STREAM_CHUNK_SIZE);
  if (maxAlloc < largeStreamBudget) {
    LOG_DBG("SCT", "Using low-memory HTML stream chunk (maxAlloc=%u)", maxAlloc);
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }
  return SECTION_HTML_STREAM_CHUNK_SIZE;
}

bool asciiEqualsIgnoreCase(const char a, const char b) {
  const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
  const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
  return la == lb;
}

size_t findImgTagStart(const std::string& window, const size_t from) {
  static const char kNeedle[] = "<img";
  if (window.size() < 4) return std::string::npos;
  for (size_t i = from; i + 4 <= window.size(); i++) {
    bool match = true;
    for (size_t j = 0; j < 4; j++) {
      if (!asciiEqualsIgnoreCase(window[i + j], kNeedle[j])) {
        match = false;
        break;
      }
    }
    // Require a tag-name boundary so <image> (SVG) is not treated as <img>.
    if (match) {
      const char next = (i + 4 < window.size()) ? window[i + 4] : ' ';
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '/' || next == '>') {
        return i;
      }
    }
  }
  return std::string::npos;
}

// Pull the value of the src attribute out of a raw <img ...> tag. The scan
// sees raw markup (no expat entity decoding), so decode &amp; — the only
// entity that legitimately shows up inside intra-zip image URLs.
std::string imgTagSrcValue(const std::string& tag) {
  for (size_t i = 1; i + 4 < tag.size(); i++) {
    if (!asciiEqualsIgnoreCase(tag[i], 's') || !asciiEqualsIgnoreCase(tag[i + 1], 'r') ||
        !asciiEqualsIgnoreCase(tag[i + 2], 'c')) {
      continue;
    }
    // Attribute-name boundary: previous char must be whitespace (rejects data-src).
    const char prev = tag[i - 1];
    if (prev != ' ' && prev != '\t' && prev != '\n' && prev != '\r') continue;

    size_t p = i + 3;
    while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t' || tag[p] == '\n' || tag[p] == '\r')) p++;
    if (p >= tag.size() || tag[p] != '=') continue;
    p++;
    while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t' || tag[p] == '\n' || tag[p] == '\r')) p++;
    if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) continue;
    const char quote = tag[p];
    const size_t valueStart = p + 1;
    const size_t valueEnd = tag.find(quote, valueStart);
    if (valueEnd == std::string::npos) return "";

    std::string value = tag.substr(valueStart, valueEnd - valueStart);
    size_t amp;
    while ((amp = value.find("&amp;")) != std::string::npos) {
      value.replace(amp, 5, "&");
    }
    return value;
  }
  return "";
}

// Extract every <img src> in the chapter HTML into the section image cache
// before layout begins, while the heap still has room for the 32KB zip
// inflate dictionary. The parser then finds each image already cached and
// performs no zip inflation mid-parse, so its low-memory floors can drop the
// dictionary reserve and spend that heap on layout instead. Individual image
// failures are non-fatal (the parser retries on cache miss and falls back to
// alt text); returns false only when the HTML itself could not be scanned.
bool preExtractSectionImages(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const int fontId,
                             const std::string& parsePath, const std::string& contentBase,
                             const std::string& imageBasePath) {
  HalFile html;
  if (!Storage.openFileForRead("SCT", parsePath, html)) {
    LOG_ERR("SCT", "Image pre-pass failed to open %s", parsePath.c_str());
    return false;
  }

  // One inflate scratch lease shared by every extraction in this pass.
  auto inflateScratch = acquireSectionZipInflateScratch(renderer, fontId, "section image pre-pass");

  constexpr size_t kReadChunk = 1024;
  constexpr size_t kMaxTagBytes = 3072;  // pathological tags longer than this are skipped
  std::string window;
  window.reserve(kReadChunk + kMaxTagBytes);
  auto chunk = makeUniqueNoThrow<char[]>(kReadChunk);
  if (!chunk) {
    html.close();
    return false;
  }
  int extracted = 0;
  bool moreData = true;

  while (moreData) {
    const int len = html.read(chunk.get(), kReadChunk);
    if (len <= 0) {
      moreData = false;
    } else {
      window.append(chunk.get(), static_cast<size_t>(len));
    }

    size_t scanned = 0;
    while (true) {
      const size_t tagStart = findImgTagStart(window, scanned);
      if (tagStart == std::string::npos) {
        // Keep a small tail in case "<img" straddles the chunk boundary.
        const size_t keep = window.size() < 4 ? window.size() : 3;
        window.erase(0, window.size() - keep);
        break;
      }
      const size_t tagEnd = window.find('>', tagStart);
      if (tagEnd == std::string::npos) {
        if (moreData && window.size() - tagStart < kMaxTagBytes) {
          window.erase(0, tagStart);  // carry the partial tag, read more
        } else {
          window.erase(0, tagStart + 4);  // oversized/unterminated tag: skip it
        }
        break;
      }

      const std::string tag = window.substr(tagStart, tagEnd - tagStart + 1);
      scanned = 0;
      window.erase(0, tagEnd + 1);

      const std::string src = imgTagSrcValue(tag);
      if (src.empty()) continue;
      const std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(contentBase + src));
      const size_t dot = resolvedPath.rfind('.');
      if (dot != std::string::npos) {
        std::string ext = resolvedPath.substr(dot);
        for (char& c : ext) {
          if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (ext == ".svg" || ext == ".svgz") continue;
      }
      if (!ImageDecoderFactory::isFormatSupported(resolvedPath)) continue;

      const std::string cachedImagePath = ChapterHtmlSlimParser::cachedImagePathForSource(imageBasePath, resolvedPath);
      if (Storage.exists(cachedImagePath.c_str())) {
        // Treat a zero-byte leftover from an interrupted extraction as a miss.
        HalFile existing;
        bool hasContent = false;
        if (Storage.openFileForRead("SCT", cachedImagePath, existing)) {
          hasContent = existing.size() > 0;
          existing.close();
        }
        if (hasContent) continue;
      }

      HalFile out;
      if (!Storage.openFileForWrite("SCT", cachedImagePath, out)) continue;
      const bool ok = epub->readItemContentsToStream(resolvedPath, out, 1024);
      out.flush();
      out.close();
      if (!ok) {
        Storage.remove(cachedImagePath.c_str());
        LOG_ERR("SCT", "Image pre-pass failed to extract %s", resolvedPath.c_str());
        continue;
      }
      extracted++;
    }
  }

  html.close();
  if (extracted > 0) {
    LOG_DBG("SCT", "Image pre-pass extracted %d image(s) (free=%u, maxAlloc=%u)", extracted, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }
  return true;
}
}  // namespace

Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer,
                 const char* cacheSuffix)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + (cacheSuffix ? cacheSuffix : "") +
               ".bin") {}

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }
  if (!page) {
    LOG_ERR("SCT", "Cannot write null page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  const uint32_t serializeStart = millis();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed (pos=%lu, serialize=%lums, free=%u, maxAlloc=%u)", pageCount,
          static_cast<unsigned long>(position), static_cast<unsigned long>(millis() - serializeStart),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  pageCount++;
  return position;
}

bool Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                     const uint16_t viewportWidth, const uint16_t viewportHeight,
                                     const bool hyphenationEnabled, const bool embeddedStyle,
                                     const uint8_t imageRendering, const bool bionicReadingEnabled,
                                     const bool guideReadingEnabled, const EpubRenderMode renderMode) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return false;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_CACHE_MAGIC) + sizeof(SECTION_FILE_VERSION) + sizeof(fontId) +
                                   sizeof(lineCompression) + sizeof(extraParagraphSpacing) +
                                   sizeof(forceParagraphIndents) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(bionicReadingEnabled) +
                                   sizeof(guideReadingEnabled) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  return serialization::tryWritePod(file, SECTION_CACHE_MAGIC) &&
         serialization::tryWritePod(file, SECTION_FILE_VERSION) && serialization::tryWritePod(file, fontId) &&
         serialization::tryWritePod(file, lineCompression) && serialization::tryWritePod(file, extraParagraphSpacing) &&
         serialization::tryWritePod(file, forceParagraphIndents) &&
         serialization::tryWritePod(file, paragraphAlignment) && serialization::tryWritePod(file, viewportWidth) &&
         serialization::tryWritePod(file, viewportHeight) && serialization::tryWritePod(file, hyphenationEnabled) &&
         serialization::tryWritePod(file, embeddedStyle) && serialization::tryWritePod(file, imageRendering) &&
         serialization::tryWritePod(file, bionicReadingEnabled) &&
         serialization::tryWritePod(file, guideReadingEnabled) &&
         serialization::tryWritePod(file, static_cast<uint8_t>(renderMode)) &&
         serialization::tryWritePod(file,
                                    pageCount) &&  // Placeholder for page count (will be initially 0, patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Placeholder for LUT offset (patched later)
         serialization::tryWritePod(file,
                                    static_cast<uint32_t>(0)) &&  // Placeholder for anchor map offset (patched later)
         serialization::tryWritePod(
             file,
             static_cast<uint32_t>(0)) &&  // Placeholder for paragraph LUT offset (patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                              const uint16_t viewportWidth, const uint16_t viewportHeight,
                              const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                              const bool bionicReadingEnabled, const bool guideReadingEnabled,
                              const EpubRenderMode renderMode) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint32_t magic;
    if (!serialization::tryReadPod(file, magic)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read cache magic");
      clearCache();
      return false;
    }
    if (magic != SECTION_CACHE_MAGIC) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: cache magic mismatch");
      clearCache();
      return false;
    }

    uint8_t version;
    if (!serialization::tryReadPod(file, version)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read version");
      clearCache();
      return false;
    }
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    bool fileForceParagraphIndents;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileBionicReadingEnabled;
    bool fileGuideReadingEnabled;
    uint8_t fileRenderMode;
    if (!serialization::tryReadPod(file, fileFontId) || !serialization::tryReadPod(file, fileLineCompression) ||
        !serialization::tryReadPod(file, fileExtraParagraphSpacing) ||
        !serialization::tryReadPod(file, fileForceParagraphIndents) ||
        !serialization::tryReadPod(file, fileParagraphAlignment) ||
        !serialization::tryReadPod(file, fileViewportWidth) || !serialization::tryReadPod(file, fileViewportHeight) ||
        !serialization::tryReadPod(file, fileHyphenationEnabled) ||
        !serialization::tryReadPod(file, fileEmbeddedStyle) || !serialization::tryReadPod(file, fileImageRendering) ||
        !serialization::tryReadPod(file, fileBionicReadingEnabled) ||
        !serialization::tryReadPod(file, fileGuideReadingEnabled) || !serialization::tryReadPod(file, fileRenderMode)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated section header");
      clearCache();
      return false;
    }

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || forceParagraphIndents != fileForceParagraphIndents ||
        paragraphAlignment != fileParagraphAlignment || viewportWidth != fileViewportWidth ||
        viewportHeight != fileViewportHeight || hyphenationEnabled != fileHyphenationEnabled ||
        embeddedStyle != fileEmbeddedStyle || imageRendering != fileImageRendering ||
        bionicReadingEnabled != fileBionicReadingEnabled || guideReadingEnabled != fileGuideReadingEnabled ||
        static_cast<uint8_t>(renderMode) != fileRenderMode) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  if (!serialization::tryReadPod(file, pageCount)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing page count");
    clearCache();
    return false;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                                const bool bionicReadingEnabled, const bool guideReadingEnabled,
                                const std::function<void()>& popupFn, bool* imagesWereSuppressed,
                                bool* layoutAbortedForLowMemory, const EpubRenderMode renderMode,
                                const SectionBuildOptions buildOptions) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto tmpSectionPath = filePath + ".tmp";
  pageCount = 0;
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = false;
  const bool effectiveBionicReadingEnabled = bionicReadingEnabled;
  const bool effectiveGuideReadingEnabled = guideReadingEnabled;
  LOG_DBG("SCT",
          "Create section start: spine=%d mode=%u preview=%u viewport=%ux%u image=%u bionic=%u guide=%u free=%u "
          "maxAlloc=%u",
          spineIndex, static_cast<unsigned>(renderMode), buildOptions.isPreview() ? 1U : 0U, viewportWidth,
          viewportHeight, imageRendering, effectiveBionicReadingEnabled, effectiveGuideReadingEnabled,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds (below), so
  // future rebuilds can skip the multi-second inflate. If htmlPath exists it is known-complete.
  const bool reusedHtml = Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  const auto cleanupTempHtml = [&]() {
    if (!htmlCached && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  };
  if (reusedHtml) {
    LOG_DBG("SCT", "Reusing cached HTML %s", htmlPath.c_str());
  } else {
    Storage.mkdir(htmlDir.c_str());

    // Retry logic for SD card timing issues
    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      const size_t htmlStreamChunkSize = sectionHtmlStreamChunkSize(buildOptions.isPreview());
      {
        auto zipInflateScratch = acquireSectionZipInflateScratch(renderer, fontId, "section one-shot HTML inflate");
        streamed = epub->readItemContentsToStream(localPath, tmpHtml, htmlStreamChunkSize);
      }
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      return false;
    }

    LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes, free=%u, maxAlloc=%u)", tmpHtmlPath.c_str(), fileSize,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // Promote to the persistent HTML cache immediately -- the inflate is complete, and caching it
    // lets future section rebuilds skip re-inflation. If the rename fails we just parse the temp.
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }
  const std::string& parsePath = htmlCached ? htmlPath : tmpHtmlPath;

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    cleanupTempHtml();
    return false;
  }
  if (!writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents, paragraphAlignment,
                              viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                              effectiveBionicReadingEnabled, effectiveGuideReadingEnabled, renderMode)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  // 1024 entries is 8 KB. Stack is too small, and std::vector growth in the page callback can abort on OOM.
  uint16_t lutCapacity = INITIAL_SECTION_PAGE_LUT_ENTRIES;
  auto lut = makeUniqueNoThrow<PageLutEntry[]>(lutCapacity);
  if (!lut) {
    LOG_ERR("SCT", "Failed to allocate page LUT (%u bytes)", static_cast<unsigned>(sizeof(PageLutEntry) * lutCapacity));
    if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  uint16_t lutCount = 0;
  bool pageCompletionFailed = false;

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u partial=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, cssParser->isCachePartial() ? 1U : 0U, static_cast<unsigned>(cssParser->ruleCount()),
              cssHeapBefore.freeHeap, cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = buildOptions.isPreview() ? -1 : epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, parsePath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, effectiveBionicReadingEnabled,
      effectiveGuideReadingEnabled,
      [this, &lut, &lutCapacity, &lutCount, &pageCompletionFailed, layoutAbortedForLowMemory](
          std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        if (pageCompletionFailed) {
          return;
        }
        if (lutCount == UINT16_MAX) {
          LOG_ERR("SCT", "Section page count exceeded cache format limit");
          pageCompletionFailed = true;
          return;
        }
        if (!ensurePageLutCapacity(lut, lutCapacity, lutCount)) {
          LOG_ERR("SCT", "Failed to grow section page LUT from %u entries", lutCapacity);
          if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
          pageCompletionFailed = true;
          return;
        }
        const uint32_t fileOffset = this->onPageComplete(std::move(page));
        if (fileOffset == 0) {
          pageCompletionFailed = true;
          return;
        }
        lut[lutCount++] = {fileOffset, paragraphIndex, listItemIndex};
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser, renderMode,
      buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{}, buildOptions.previewMaxPages);
  Hyphenator::setPreferredLanguage(epub->getLanguage());

  // Extract the chapter's images up front, while the heap still has room for
  // the 32KB zip inflate dictionary. The parser then finds every image
  // already cached and performs no zip inflation mid-parse, letting its
  // low-memory floors drop the dictionary reserve and spend that heap on
  // layout (see preExtractSectionImages). When images are disabled the parse
  // does no zip work either, so the relaxed floors apply trivially. Previews
  // cap at a few pages and keep the conservative floors instead.
  bool imagesPreExtracted = false;
  if (!buildOptions.isPreview()) {
    if (imageRendering == 1) {
      imagesPreExtracted = true;
    } else {
      imagesPreExtracted = preExtractSectionImages(epub, renderer, fontId, parsePath, contentBase, imageBasePath);
    }
    visitor.setImagesPreExtracted(imagesPreExtracted);
  }

  LOG_DBG("SCT", "Parser start: spine=%d free=%u maxAlloc=%u preExtracted=%u", spineIndex, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap(), imagesPreExtracted ? 1U : 0U);
  const bool success = visitor.parseAndBuildPages();
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", spineIndex, success, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (imagesWereSuppressed) *imagesWereSuppressed = visitor.wasLowMemoryFallbackTriggered();
  if (layoutAbortedForLowMemory) {
    *layoutAbortedForLowMemory = *layoutAbortedForLowMemory || visitor.wasLowMemoryAbortTriggered();
  }

  if (!htmlCached) {
    if (success || pageCompletionFailed) {
      // Promote the freshly unzipped HTML to the persistent cache so future rebuilds (e.g. after a
      // settings change invalidates the layout caches) can skip zip inflation. If promotion fails,
      // drop the temp file; the section build can still continue from the already-open source.
      if (!Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
        LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
        Storage.remove(tmpHtmlPath.c_str());
      }
    } else {
      // Parse failed on a freshly unzipped file -- discard it rather than caching a bad source.
      Storage.remove(tmpHtmlPath.c_str());
    }
  }

  if (!success || pageCompletionFailed) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (uint16_t i = 0; i < lutCount; i++) {
    if (lut[i].fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    if (!serialization::tryWritePod(file, lut[i].fileOffset)) {
      hasFailedLutRecords = true;
      break;
    }
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(anchors.size()))) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  for (const auto& [anchor, page] : anchors) {
    if (!serialization::tryWriteString(file, anchor) || !serialization::tryWritePod(file, page)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t paragraphLutOffset = file.position();
  if (!serialization::tryWritePod(file, lutCount)) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  for (uint16_t i = 0; i < lutCount; i++) {
    if (!serialization::tryWritePod(file, lut[i].paragraphIndex)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (uint16_t i = 0; i < lutCount; i++) {
    if (!serialization::tryWritePod(file, lut[i].listItemIndex)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, paragraphLutOffset, and liLutOffset.
  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount)) ||
      !serialization::tryWritePod(file, pageCount) || !serialization::tryWritePod(file, lutOffset) ||
      !serialization::tryWritePod(file, anchorMapOffset) || !serialization::tryWritePod(file, paragraphLutOffset) ||
      !serialization::tryWritePod(file, liLutFileOffset) || !file.sync()) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!Storage.rename(tmpSectionPath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to promote temp section cache into place");
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (cssParser) {
    cssParser->clear();
  }
  LOG_DBG("SCT", "Create section done: spine=%d pages=%u free=%u maxAlloc=%u", spineIndex, pageCount, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  return true;
}

bool Section::hasHtmlCache() const {
  const std::string htmlPath = epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!file) {
    if (!Storage.openFileForRead("SCT", filePath, file)) {
      return nullptr;
    }
  }

  auto closeAndReturnNull = [this]() -> std::unique_ptr<Page> {
    file.close();
    return nullptr;
  };

  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 4)) {
    return closeAndReturnNull();
  }
  uint32_t lutOffset;
  if (!serialization::tryReadPod(file, lutOffset) || !file.seek(lutOffset + sizeof(uint32_t) * currentPage)) {
    return closeAndReturnNull();
  }
  uint32_t pagePos;
  if (!serialization::tryReadPod(file, pagePos) || !file.seek(pagePos)) {
    return closeAndReturnNull();
  }

  auto page = Page::deserialize(file);
  file.close();
  return page;
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = loadPageFromSectionFile();
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); i++) {
            if (!fullText.empty()) fullText += " ";
            fullText += block.wordText(i);
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  uint32_t magic;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC) {
    return std::nullopt;
  }
  uint8_t version;
  if (!serialization::tryReadPod(f, version)) {
    return std::nullopt;
  }
  if (version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t anchorMapOffset;
  if (!serialization::tryReadPod(f, anchorMapOffset)) {
    return std::nullopt;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(anchorMapOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    if (!serialization::tryReadString(f, key) || !serialization::tryReadPod(f, page)) {
      return std::nullopt;
    }
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    if (!serialization::tryReadPod(f, pagePIdx)) {
      return std::nullopt;
    }
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t pIdx;
  if (!serialization::tryReadPod(f, pIdx)) {
    return std::nullopt;
  }
  return pIdx;
}

std::optional<uint16_t> Section::getListItemIndexForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = liLutOffset + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t liIdx;
  if (!serialization::tryReadPod(f, liIdx)) {
    return std::nullopt;
  }
  return liIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset)) {
    return std::nullopt;
  }
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    if (!serialization::tryReadPod(f, pageLiIdx)) {
      return std::nullopt;
    }
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
