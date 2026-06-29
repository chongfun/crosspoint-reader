#include "Section.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <Serialization.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

#include "AsciiCase.h"
#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
// v42: page LUT entries include offsets to compact text records used by search.
constexpr uint8_t SECTION_FILE_VERSION = 42;
constexpr uint16_t INITIAL_SECTION_PAGE_LUT_ENTRIES = 1024;
constexpr uint32_t HEADER_SIZE = sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) +
                                 sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                 sizeof(bool) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);

// The header ends with a fixed trailer written last and patched after layout
// (see writeSectionFileHeader): a uint16_t pageCount followed by four uint32_t
// offsets in this order — page LUT, anchor map, paragraph LUT, list-item LUT.
// Name each field's absolute seek position so readers/patchers don't open-code
// `HEADER_SIZE - sizeof(uint32_t) * N` arithmetic (and risk an off-by-one).
constexpr size_t LI_LUT_OFFSET_POS = HEADER_SIZE - sizeof(uint32_t);
constexpr size_t PARAGRAPH_LUT_OFFSET_POS = HEADER_SIZE - sizeof(uint32_t) * 2;
constexpr size_t ANCHOR_MAP_OFFSET_POS = HEADER_SIZE - sizeof(uint32_t) * 3;
constexpr size_t PAGE_LUT_OFFSET_POS = HEADER_SIZE - sizeof(uint32_t) * 4;
constexpr size_t PAGE_COUNT_POS = PAGE_LUT_OFFSET_POS - sizeof(uint16_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint32_t searchTextOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};
bool ensurePageLutCapacity(std::unique_ptr<PageLutEntry[]>& lut, uint16_t& lutCapacity, const uint16_t lutCount) {
  if (lutCount < lutCapacity) return true;
  if (lutCapacity == UINT16_MAX) return false;

  uint32_t nextCapacity = static_cast<uint32_t>(lutCapacity) * 2U;
  if (nextCapacity > UINT16_MAX) {
    nextCapacity = UINT16_MAX;
  }

  auto grown = makeUniqueNoThrow<PageLutEntry[]>(nextCapacity);
  if (!grown) return false;

  for (uint16_t i = 0; i < lutCount; i++) {
    grown[i] = lut[i];
  }
  lut = std::move(grown);
  lutCapacity = static_cast<uint16_t>(nextCapacity);
  return true;
}

static_assert(sizeof(PageLutEntry) == 12, "Unexpected PageLutEntry padding changes the transient RAM budget");

struct ScopedSectionFile {
  HalFile& file;
  bool openedLocally;
  ScopedSectionFile(HalFile& f, const std::string& path) : file(f), openedLocally(false) {
    if (!file) {
      if (Storage.openFileForRead("SCT", path, file)) {
        openedLocally = true;
      }
    }
  }
  ~ScopedSectionFile() {
    if (openedLocally) {
      file.close();
    }
  }
  bool ok() const { return static_cast<bool>(file); }
};

// On-disk page LUT stride: only pageOffset and searchTextOffset are stored
// inline; paragraphIndex and listItemIndex are written to separate LUTs.
constexpr size_t PAGE_LUT_ENTRY_SIZE = sizeof(uint32_t) * 2;
// Bind the stride to the two inline offset fields so adding or resizing an
// inline LUT field can't silently desync it from the write/read sites.
static_assert(PAGE_LUT_ENTRY_SIZE == sizeof(PageLutEntry::fileOffset) + sizeof(PageLutEntry::searchTextOffset),
              "On-disk page-LUT stride must match the inline offset fields");
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page, uint32_t& searchTextOffset) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }
  if (!page) {
    LOG_ERR("SCT", "Cannot write null page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  searchTextOffset = file.position();
  if (!page->serializeSearchText(file)) {
    LOG_ERR("SCT", "Failed to serialize search text for page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed (pos=%lu, free=%u, maxAlloc=%u)", pageCount, static_cast<unsigned long>(position),
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
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";
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

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes, free=%u, maxAlloc=%u)", tmpHtmlPath.c_str(), fileSize,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    return false;
  }
  if (!writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents, paragraphAlignment,
                              viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                              effectiveBionicReadingEnabled, effectiveGuideReadingEnabled, renderMode)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  // 1024 entries is 12 KB. Stack is too small, and std::vector growth in the page callback can abort on OOM.
  uint16_t lutCapacity = INITIAL_SECTION_PAGE_LUT_ENTRIES;
  auto lut = makeUniqueNoThrow<PageLutEntry[]>(lutCapacity);
  if (!lut) {
    LOG_ERR("SCT", "Failed to allocate page LUT (%u bytes)", static_cast<unsigned>(sizeof(PageLutEntry) * lutCapacity));
    if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
    file.close();
    Storage.remove(tmpSectionPath.c_str());
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
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
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
        uint32_t searchTextOffset = 0;
        const uint32_t fileOffset = this->onPageComplete(std::move(page), searchTextOffset);
        if (fileOffset == 0) {
          pageCompletionFailed = true;
          return;
        }
        lut[lutCount++] = {fileOffset, searchTextOffset, paragraphIndex, listItemIndex};
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser, renderMode,
      buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{}, buildOptions.previewMaxPages);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  LOG_DBG("SCT", "Parser start: spine=%d free=%u maxAlloc=%u", spineIndex, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  success = visitor.parseAndBuildPages();
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", spineIndex, success, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (imagesWereSuppressed) *imagesWereSuppressed = visitor.wasLowMemoryFallbackTriggered();
  if (layoutAbortedForLowMemory) {
    *layoutAbortedForLowMemory = *layoutAbortedForLowMemory || visitor.wasLowMemoryAbortTriggered();
  }

  Storage.remove(tmpHtmlPath.c_str());
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
    if (!serialization::tryWritePod(file, lut[i].fileOffset) ||
        !serialization::tryWritePod(file, lut[i].searchTextOffset)) {
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
  if (!file.seek(PAGE_COUNT_POS) || !serialization::tryWritePod(file, pageCount) ||
      !serialization::tryWritePod(file, lutOffset) || !serialization::tryWritePod(file, anchorMapOffset) ||
      !serialization::tryWritePod(file, paragraphLutOffset) || !serialization::tryWritePod(file, liLutFileOffset) ||
      !file.sync()) {
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

bool Section::readPageLutOffset(uint32_t& lutOffset) {
  if (!file.seek(PAGE_LUT_OFFSET_POS)) {
    return false;
  }
  return file.read(reinterpret_cast<uint8_t*>(&lutOffset), sizeof(lutOffset)) == sizeof(lutOffset);
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  ScopedSectionFile sf(file, filePath);
  if (!sf.ok()) {
    return nullptr;
  }

  const uint32_t fileSize = file.size();
  if (fileSize < HEADER_SIZE) {
    LOG_ERR("SCT", "Section cache header is truncated");
    return nullptr;
  }

  uint32_t lutOffset = 0;
  if (!readPageLutOffset(lutOffset)) {
    LOG_ERR("SCT", "Failed to read page LUT offset");
    return nullptr;
  }

  if (!file.seek(PAGE_COUNT_POS)) {
    LOG_ERR("SCT", "Failed to seek to page count");
    return nullptr;
  }
  uint16_t headerPageCount = 0;
  if (!serialization::tryReadPod(file, headerPageCount)) {
    LOG_ERR("SCT", "Failed to read page count from header");
    return nullptr;
  }

  // Validate LUT-derived offsets against the file before trusting them (mirrors
  // scanForward). Compute in 64-bit so a corrupt (huge) lutOffset cannot
  // wrap the uint32 sum into a small in-bounds value.
  if (lutOffset == 0 || currentPage < 0 || static_cast<uint32_t>(currentPage) >= headerPageCount) {
    LOG_ERR("SCT", "Invalid page LUT request");
    return nullptr;
  }
  const uint64_t entryOffset = static_cast<uint64_t>(lutOffset) +
                               static_cast<uint64_t>(PAGE_LUT_ENTRY_SIZE) * static_cast<uint32_t>(currentPage);
  if (entryOffset > fileSize || fileSize - entryOffset < PAGE_LUT_ENTRY_SIZE) {
    LOG_ERR("SCT", "Invalid page LUT entry");
    return nullptr;
  }
  if (!file.seek(static_cast<size_t>(entryOffset))) {
    LOG_ERR("SCT", "Failed to seek to page LUT entry");
    return nullptr;
  }
  uint32_t pagePos = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&pagePos), sizeof(pagePos)) != sizeof(pagePos) || pagePos >= fileSize) {
    LOG_ERR("SCT", "Failed to read page offset");
    return nullptr;
  }
  if (!file.seek(pagePos)) {
    LOG_ERR("SCT", "Failed to seek to page record");
    return nullptr;
  }

  return Page::deserialize(file);
}

void Section::rebuildFilePathForSpine() {
  // Re-append the "<spineIndex><cacheSuffix>.bin" suffix onto the cached prefix in place.
  // snprintf into a stack buffer avoids std::to_string's heap temporary, and
  // resize()+append() reuse filePath's reserved capacity (no reallocation).
  char numBuf[12];
  const int len = snprintf(numBuf, sizeof(numBuf), "%d", spineIndex);
  filePath.resize(sectionPathPrefixLen);
  if (len > 0) {
    filePath.append(numBuf, static_cast<size_t>(len));
  }
  if (!cacheSuffix.empty()) {
    filePath.append(cacheSuffix);
  }
  filePath.append(".bin");
}

void Section::closeSearchState() {
  if (file) {
    file.close();
  }
  searchHeaderReady = false;
}

void Section::resetForSpine(const int newSpineIndex) {
  closeSearchState();
  spineIndex = newSpineIndex;
  rebuildFilePathForSpine();
  pageCount = 0;
  currentPage = 0;
}
bool Section::ensureSearchHeader() {
  if (searchHeaderReady) {
    return true;
  }

  // Open the member file handle lazily on the first call. It stays open for
  // all pages in this section; resetForSpine() closes it when advancing.
  if (!file) {
    if (!Storage.openFileForRead("SCT", filePath, file)) {
      return false;
    }
  }

  const uint32_t fileSize = file.size();
  if (fileSize < HEADER_SIZE) {
    LOG_ERR("SCT", "Search failed: section cache header is truncated");
    // Release the handle so the corrupt cache can be invalidated/rebuilt; the
    // next call reopens lazily (searchHeaderReady stays false).
    closeSearchState();
    return false;
  }

  uint32_t lutOffset = 0;
  if (!readPageLutOffset(lutOffset)) {
    LOG_ERR("SCT", "Search failed: could not read page LUT offset");
    closeSearchState();
    return false;
  }

  searchFileSize = fileSize;
  searchLutOffset = lutOffset;
  searchHeaderReady = true;
  return true;
}

Section::ScanResult Section::scanForward(uint16_t startPage, uint16_t endPage, SearchMatcher& matcher) {
  if (startPage >= pageCount || startPage >= endPage) {
    return {ScanStatus::NoMatch, -1};
  }
  if (endPage > pageCount) {
    endPage = pageCount;
  }

  // File size and page-LUT offset are invariant per section; read them once.
  if (!ensureSearchHeader()) {
    return {ScanStatus::CorruptCache, -1};
  }
  const uint32_t fileSize = searchFileSize;
  const uint32_t lutOffset = searchLutOffset;

  const uint16_t count = endPage - startPage;
  const uint64_t entryOffset =
      static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(PAGE_LUT_ENTRY_SIZE) * startPage;
  if (lutOffset == 0 || entryOffset > fileSize ||
      fileSize - entryOffset < static_cast<uint64_t>(PAGE_LUT_ENTRY_SIZE) * count) {
    LOG_ERR("SCT", "Search failed: invalid page LUT entry range");
    closeSearchState();
    return {ScanStatus::CorruptCache, -1};
  }

  // Batch read the LUT entries for the requested page range into a reused
  // buffer, allocated (or grown) once with nothrow ownership so repeated chunked
  // scans do not churn the heap and an allocation failure is a recoverable
  // search error rather than an abort.
  const size_t lutBytes = static_cast<size_t>(count) * PAGE_LUT_ENTRY_SIZE;
  if (searchLutBufCapacity < lutBytes) {
    searchLutBuf = makeUniqueNoThrow<uint8_t[]>(lutBytes);
    if (!searchLutBuf) {
      searchLutBufCapacity = 0;
      LOG_ERR("SCT", "Search failed: OOM for page LUT buffer (%u bytes)", static_cast<unsigned>(lutBytes));
      closeSearchState();
      return {ScanStatus::IoError, -1};
    }
    searchLutBufCapacity = lutBytes;
  }
  if (!file.seek(static_cast<size_t>(entryOffset))) {
    LOG_ERR("SCT", "Search failed: could not seek to page LUT entries");
    closeSearchState();
    return {ScanStatus::IoError, -1};
  }
  if (file.read(searchLutBuf.get(), lutBytes) != lutBytes) {
    LOG_ERR("SCT", "Search failed: could not read page LUT entries");
    closeSearchState();
    return {ScanStatus::CorruptCache, -1};
  }

  // Sequentially read the text records
  std::array<uint8_t, 64> buffer;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t searchTextOffset = 0;
    // searchTextOffset is the 2nd uint32_t in the LUT entry
    memcpy(&searchTextOffset, searchLutBuf.get() + i * PAGE_LUT_ENTRY_SIZE + sizeof(uint32_t), sizeof(uint32_t));
    if (searchTextOffset > fileSize || fileSize - searchTextOffset < sizeof(uint32_t)) {
      LOG_ERR("SCT", "Search failed: invalid text record offset");
      closeSearchState();
      return {ScanStatus::CorruptCache, -1};
    }

    if (!file.seek(searchTextOffset)) {
      LOG_ERR("SCT", "Search failed: could not seek to text record");
      closeSearchState();
      return {ScanStatus::IoError, -1};
    }

    uint32_t remaining = 0;
    if (file.read(reinterpret_cast<uint8_t*>(&remaining), sizeof(remaining)) != sizeof(remaining) ||
        remaining > fileSize - searchTextOffset - sizeof(uint32_t)) {
      LOG_ERR("SCT", "Search failed: invalid text record length");
      closeSearchState();
      return {ScanStatus::CorruptCache, -1};
    }

    // A page with no searchable text (e.g. image-only) is a content discontinuity,
    // so drop any carried partial match rather than bridging across it.
    if (remaining == 0) {
      matcher.reset();
      continue;
    }

    while (remaining > 0) {
      const size_t chunkSize = std::min<size_t>(buffer.size(), remaining);
      if (file.read(buffer.data(), chunkSize) != chunkSize) {
        LOG_ERR("SCT", "Search failed: truncated text record");
        closeSearchState();
        return {ScanStatus::CorruptCache, -1};
      }
      remaining -= chunkSize;

      for (size_t j = 0; j < chunkSize; ++j) {
        if (matcher.feed(buffer[j]) > 0) {
          return {ScanStatus::Match, static_cast<int>(startPage + i)};
        }
      }
    }
  }

  return {ScanStatus::NoMatch, -1};
}

std::optional<uint16_t> Section::getCachedPageCount() {
  ScopedSectionFile sf(file, filePath);
  if (!sf.ok()) {
    return std::nullopt;
  }

  const uint32_t fileSize = file.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  if (!file.seek(PAGE_COUNT_POS)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
    return std::nullopt;
  }
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) {
  ScopedSectionFile sf(file, filePath);
  if (!sf.ok()) {
    return std::nullopt;
  }

  const uint32_t fileSize = file.size();
  if (!file.seek(ANCHOR_MAP_OFFSET_POS)) {
    return std::nullopt;
  }
  uint32_t anchorMapOffset;
  if (!serialization::tryReadPod(file, anchorMapOffset)) {
    return std::nullopt;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  if (!file.seek(anchorMapOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
    return std::nullopt;
  }
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    if (!serialization::tryReadString(file, key) || !serialization::tryReadPod(file, page)) {
      return std::nullopt;
    }
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) {
  ScopedSectionFile sf(file, filePath);
  if (!sf.ok()) {
    return std::nullopt;
  }

  const uint32_t fileSize = file.size();
  if (!file.seek(PARAGRAPH_LUT_OFFSET_POS)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(file, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!file.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
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
    if (!serialization::tryReadPod(file, pagePIdx)) {
      return std::nullopt;
    }
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) {
  ScopedSectionFile sf(file, filePath);
  if (!sf.ok()) {
    return std::nullopt;
  }

  const uint32_t fileSize = file.size();
  if (!file.seek(PARAGRAPH_LUT_OFFSET_POS)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(file, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!file.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!file.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t pIdx;
  if (!serialization::tryReadPod(file, pIdx)) {
    return std::nullopt;
  }
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) {
  ScopedSectionFile sf(file, filePath);
  if (!sf.ok()) {
    return std::nullopt;
  }

  const uint32_t fileSize = file.size();
  if (!file.seek(LI_LUT_OFFSET_POS)) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(file, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  if (!file.seek(PARAGRAPH_LUT_OFFSET_POS)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(file, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!file.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!file.seek(liLutOffset)) {
    return std::nullopt;
  }
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    if (!serialization::tryReadPod(file, pageLiIdx)) {
      return std::nullopt;
    }
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
