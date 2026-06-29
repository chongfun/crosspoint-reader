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

  // Cached section-header state for the search scan: the file size and page-LUT
  // offset are invariant per section, so they are read once when the scan file
  // is lazily opened and reused for every pageContainsText() call. Invalidated
  // by resetForSpine() (which also closes the file).
  bool searchHeaderReady = false;
  uint32_t searchFileSize = 0;
  uint32_t searchLutOffset = 0;

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
  // false on open failure or a truncated/corrupt header.
  bool ensureSearchHeader();
  void closeSearchState();
  // Rewrite filePath's numeric suffix in place for the current spineIndex,
  // reusing the buffer (no per-spine string allocation, no std::to_string).
  void rebuildFilePathForSpine();

 public:
  static constexpr size_t MAX_SEARCH_QUERY_BYTES = 64;
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
  std::string getTextFromSectionFile();

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Reuse this Section object for another spine item without another heap
  // allocation. Intended for sequential, book-wide operations such as search.
  void resetForSpine(int newSpineIndex);

  // Search forward through cached section pages from `startPage` up to `endPage`,
  // batching LUT reads and streaming text records sequentially. Returns the
  // first page index where `matcher.feed` completes a match, -1 if no match,
  // or nullopt if a cache error occurs.
  std::optional<int> scanForward(uint16_t startPage, uint16_t endPage, SearchMatcher& matcher);

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
