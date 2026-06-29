#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Epub.h"
#include "EpubRenderMode.h"
#include "SearchMatcher.h"

class Page;
class GfxRenderer;

struct SectionBuildOptions {
  const char* previewAnchor = nullptr;
  uint16_t previewMaxPages = 0;

  bool isPreview() const { return previewAnchor && previewAnchor[0] != '\0' && previewMaxPages > 0; }
};

class Section {
 public:
  // Why a forward search scan stopped. Distinguishes a structurally corrupt
  // cache (which rebuilding can repair) from a transient I/O failure or OOM
  // (which it cannot), so the caller does not delete a valid cache over a
  // momentary glitch.
  enum class ScanStatus : uint8_t {
    Match,         // a match was found; `page` holds the page index
    NoMatch,       // the requested range was scanned with no match (or was empty)
    CorruptCache,  // structurally invalid cache data; a rebuild may help
    IoError,       // seek/open failure or OOM; rebuilding will not help
  };
  struct ScanResult {
    ScanStatus status = ScanStatus::NoMatch;
    int page = -1;  // valid only when status == Match
  };

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

  std::string cacheSuffix;

  // Per-section state for the forward search scan (scanForward(), defined in
  // SectionSearch.cpp), grouped so the feature adds one member to this class
  // rather than several interleaved fields. Invalidated by resetForSpine(),
  // which closes the file via closeSearchState().
  struct SearchScanState {
    // The file size and page-LUT offset are invariant per section, so they are
    // read once when the scan file is lazily opened and reused for every scan.
    bool headerReady = false;
    uint32_t fileSize = 0;
    uint32_t lutOffset = 0;

    // Reused scratch buffer for batched page-LUT reads. Allocated once on first
    // use (nothrow) and grown only if a larger page range appears, so repeated
    // chunked scans do not churn the heap; an OOM is a recoverable search
    // failure rather than an abort. Freed when the Section is destroyed.
    std::unique_ptr<uint8_t[]> lutBuf;
    size_t lutBufCapacity = 0;
  };
  SearchScanState searchScan;

  bool writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                              uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                              bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                              bool bionicReadingEnabled, bool guideReadingEnabled, EpubRenderMode renderMode);
  uint32_t onPageComplete(std::unique_ptr<Page> page, uint32_t& searchTextOffset);
  // Seek to and read the page-LUT offset from the section header (the single
  // place that knows where that field lives). Requires the member file to be
  // open; returns false on seek/read failure.
  bool readPageLutOffset(uint32_t& lutOffset);
  // Lazily open the scan file and cache its size and page-LUT offset. Returns
  // false on failure, setting failureStatus to IoError for an open/seek/read
  // failure or CorruptCache for a truncated/malformed header.
  bool ensureSearchHeader(ScanStatus& failureStatus);
  void closeSearchState();
  // Rewrite filePath's numeric suffix in place for the current spineIndex,
  // reusing the buffer (no per-spine string allocation, no std::to_string).
  void rebuildFilePathForSpine();

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer,
                   const char* cacheSuffix = "")
      : epub(epub), spineIndex(spineIndex), renderer(renderer), cacheSuffix(cacheSuffix ? cacheSuffix : "") {
    // Build the constant "<cachePath>/sections/" prefix once and remember its
    // length; resetForSpine() then rewrites only the numeric suffix in place.
    const std::string& cachePath = this->epub->getCachePath();
    filePath.reserve(cachePath.size() + 32 + this->cacheSuffix.size());  // prefix + up to 11 digits + suffix + ".bin"
    filePath.assign(cachePath).append("/sections/");
    sectionPathPrefixLen = filePath.size();
    rebuildFilePathForSpine();
  }
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                       uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, bool bionicReadingEnabled,
                       bool guideReadingEnabled, EpubRenderMode renderMode);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                         bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, bool bionicReadingEnabled,
                         bool guideReadingEnabled, const std::function<void()>& popupFn = nullptr,
                         bool* imagesWereSuppressed = nullptr, bool* layoutAbortedForLowMemory = nullptr,
                         EpubRenderMode renderMode = EpubRenderMode::CrossInkDefault,
                         SectionBuildOptions buildOptions = {});
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount();

  // Reuse this Section object for another spine item without another heap
  // allocation. Intended for sequential, book-wide operations such as search.
  void resetForSpine(int newSpineIndex);

  // Search forward through cached section pages from `startPage` up to `endPage`,
  // batching LUT reads and streaming text records sequentially. Returns Match
  // with the first matching page index, NoMatch when the range is exhausted, or
  // a failure status distinguishing corrupt-cache from transient I/O.
  ScanResult scanForward(uint16_t startPage, uint16_t endPage, SearchMatcher& matcher);

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor);

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex);

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex);

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page);
};
