#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
 public:
  static constexpr uint8_t SECTION_FILE_VERSION = 28;
  static constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) +
                                          sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                          sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                          sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
  static constexpr size_t PAGE_LUT_ENTRY_SIZE = sizeof(uint32_t) * 2;

 private:
  std::shared_ptr<Epub> epub;
  int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  // Byte length of the constant "<cachePath>/sections/" prefix within filePath.
  // resetForSpine() truncates filePath to this length and re-appends only the
  // numeric suffix, reusing the buffer instead of allocating a new string per
  // spine transition.
  size_t sectionPathPrefixLen = 0;
  HalFile file;

  // Cached section-header state for the search scan: the file size and page-LUT
  // offset are invariant per section, so they are read once when the scan file
  // is lazily opened and reused for every pageContainsText() call. Invalidated
  // by resetForSpine() (which also closes the file).
  bool searchHeaderReady = false;
  uint32_t searchFileSize = 0;
  uint32_t searchLutOffset = 0;

  // Reused scan buffers, allocated once (nothrow) and grown as needed so repeated
  // chunked scans do not churn the heap and an allocation failure is a
  // recoverable search error rather than an abort. Not freed by closeSearchState();
  // they persist for the Section's lifetime and are reused across spines. lutBuf
  // holds one chunk's page-LUT entries; textBuf batches text-record reads to
  // minimize slow SPI transactions over the small stack array previously used.
  std::unique_ptr<uint8_t[]> searchLutBuf;
  size_t searchLutBufCapacity = 0;
  std::unique_ptr<uint8_t[]> searchTextBuf;
  size_t searchTextBufCapacity = 0;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering, bool focusReadingEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page, uint32_t& searchTextOffset);
  // Seek to and read the page-LUT offset from the section header (the single
  // place that knows where that field lives). Requires the member file to be
  // open; returns false on seek/read failure.
  bool readPageLutOffset(uint32_t& lutOffset);
  // Lazily open the scan file and cache its size and page-LUT offset. Returns
  // false on open failure or a truncated/corrupt header.
  bool ensureSearchHeader();
  // Rewrite filePath's numeric suffix in place for the current spineIndex,
  // reusing the buffer (no per-spine string allocation, no std::to_string).
  void rebuildFilePathForSpine();

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub), spineIndex(spineIndex), renderer(renderer) {
    // Build the constant "<cachePath>/sections/" prefix once and remember its
    // length; resetForSpine() then rewrites only the numeric suffix in place.
    const std::string& cachePath = this->epub->getCachePath();
    filePath.reserve(cachePath.size() + 32);  // prefix + up to 11 digits + ".bin"
    filePath.assign(cachePath).append("/sections/");
    sectionPathPrefixLen = filePath.size();
    rebuildFilePathForSpine();
  }
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, bool focusReadingEnabled);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, bool focusReadingEnabled,
                         const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();
  std::string getTextFromSectionFile();

  // Reuse this Section object for another spine item without another heap
  // allocation. Intended for sequential, book-wide operations such as search.
  void resetForSpine(int newSpineIndex);

  // Close the lazily-opened scan file and invalidate the cached header. scanForward()
  // intentionally leaves the file open between chunked scans (resetForSpine() closes
  // it when advancing spines); a one-shot caller such as the search highlighter, which
  // primes the matcher on the reader's live Section, must call this afterwards so the
  // reader does not sit on an open SD handle (only one file may be open at a time on HW).
  void closeSearchState();

  enum class ScanStatus : uint8_t {
    Match,         // a match was found; `page` holds the page index
    NoMatch,       // the requested range was scanned with no match (or was empty)
    CorruptCache,  // structurally invalid cache data; a rebuild may help
    IoError,       // seek/open failure or OOM; rebuilding will not help
  };

  struct ScanResult {
    ScanStatus status = ScanStatus::NoMatch;
    int page = -1;  // valid only when status == Match
    // Byte span of the match within `page`'s serialized search-text record
    // (inclusive), valid only when status == Match. startByte is clamped to 0
    // when the match began on an earlier page, so [matchStartByte, matchEndByte]
    // always covers the portion that lies on `page`. SearchHighlighter maps this
    // span to the page's words without re-running the matcher.
    int matchStartByte = -1;
    int matchEndByte = -1;
  };

  // Search forward through cached section pages from `startPage` up to `endPage`,
  // batching LUT reads and streaming text records sequentially. Returns Match
  // with the first matching page index, NoMatch when the range is exhausted, or
  // a failure status distinguishing corrupt-cache from transient I/O.
  ScanResult scanForward(uint16_t startPage, uint16_t endPage, class SearchMatcher& matcher);

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
