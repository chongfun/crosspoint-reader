#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "SearchMatcher.h"
#include "Section.h"

void Section::closeSearchState() {
  file.close();
  searchHeaderReady = false;
  searchFileSize = 0;
  searchLutOffset = 0;
}

Section::ScanResult Section::scanForward(uint16_t startPage, uint16_t endPage, SearchMatcher& matcher) {
  if (startPage >= pageCount || startPage >= endPage) {
    return {ScanStatus::NoMatch, -1};
  }
  if (endPage > pageCount) {
    endPage = pageCount;
  }

  if (!ensureSearchHeader()) {
    // If ensureSearchHeader failed due to missing file/bad header,
    // we'll just treat it as a corrupt cache to let the caller handle it.
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

  // We allocate a buffer for the chunk to reduce seek overhead.
  const size_t lutBytes = static_cast<size_t>(count) * PAGE_LUT_ENTRY_SIZE;
  auto lutBuf = std::make_unique<uint8_t[]>(lutBytes);
  if (!lutBuf) {
    LOG_ERR("SCT", "Search failed: OOM for page LUT buffer (%u bytes)", static_cast<unsigned>(lutBytes));
    closeSearchState();
    return {ScanStatus::IoError, -1};
  }

  if (!file.seek(static_cast<size_t>(entryOffset))) {
    LOG_ERR("SCT", "Search failed: could not seek to page LUT entries");
    closeSearchState();
    return {ScanStatus::IoError, -1};
  }
  if (file.read(lutBuf.get(), lutBytes) != lutBytes) {
    LOG_ERR("SCT", "Search failed: could not read page LUT entries");
    closeSearchState();
    return {ScanStatus::IoError, -1};
  }

  std::array<uint8_t, 64> buffer;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t searchTextOffset = 0;
    memcpy(&searchTextOffset, lutBuf.get() + i * PAGE_LUT_ENTRY_SIZE + sizeof(uint32_t), sizeof(uint32_t));
    if (searchTextOffset < HEADER_SIZE || searchTextOffset > lutOffset ||
        lutOffset - searchTextOffset < sizeof(uint32_t)) {
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
    if (file.read(reinterpret_cast<uint8_t*>(&remaining), sizeof(remaining)) != sizeof(remaining)) {
      LOG_ERR("SCT", "Search failed: could not read text record length");
      closeSearchState();
      return {ScanStatus::IoError, -1};
    }
    if (remaining > lutOffset - searchTextOffset - sizeof(uint32_t)) {
      LOG_ERR("SCT", "Search failed: invalid text record length");
      closeSearchState();
      return {ScanStatus::CorruptCache, -1};
    }

    if (remaining == 0) {
      if (matcher.hasPendingMatch()) {
        return {ScanStatus::Match, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
      }
      matcher.reset();
      continue;
    }

    if (matcher.feed(' ') < 0) {
      return {ScanStatus::Match, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
    }

    uint32_t pageBytePos = 0;
    while (remaining > 0) {
      const size_t chunkSize = std::min<size_t>(buffer.size(), remaining);
      if (file.read(buffer.data(), chunkSize) != chunkSize) {
        LOG_ERR("SCT", "Search failed: truncated text record");
        closeSearchState();
        return {ScanStatus::IoError, -1};
      }
      remaining -= chunkSize;

      for (size_t j = 0; j < chunkSize; ++j) {
        const int signal = matcher.feed(buffer[j]);
        if (signal > 0) {
          const int endByte = static_cast<int>(pageBytePos);
          const int startByte = (pageBytePos + 1 >= static_cast<uint32_t>(signal))
                                    ? static_cast<int>(pageBytePos + 1 - static_cast<uint32_t>(signal))
                                    : 0;
          matcher.setPendingMatchSpan(static_cast<int>(startPage + i), startByte, endByte);
        } else if (signal < 0) {
          return {ScanStatus::Match, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
        }
        ++pageBytePos;
      }
    }

    if (matcher.hasPendingMatch() && !matcher.isHyphenPending()) {
      return {ScanStatus::Match, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
    }
  }

  return {ScanStatus::NoMatch, -1};
}
