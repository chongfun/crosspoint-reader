#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
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

  // ensureSearchHeader distinguishes a transient I/O failure from a corrupt
  // header so we do not delete a valid cache over a momentary glitch.
  ScanStatus headerFailure = ScanStatus::CorruptCache;
  if (!ensureSearchHeader(headerFailure)) {
    return {headerFailure, -1};
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

  // Batch-read the chunk's LUT entries into a reused buffer, allocated (or grown)
  // once with nothrow ownership so repeated chunked scans do not churn the heap
  // and an allocation failure is a recoverable search error rather than an abort.
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
    return {ScanStatus::IoError, -1};
  }

  // Allocate a larger reused heap buffer to batch text reads, drastically
  // reducing slow SPI transactions over the small 64-byte stack array previously
  // used. Allocated once (nothrow) and reused across chunks and spines.
  constexpr size_t TEXT_BUF_SIZE = 2048;
  if (searchTextBufCapacity < TEXT_BUF_SIZE) {
    searchTextBuf = makeUniqueNoThrow<uint8_t[]>(TEXT_BUF_SIZE);
    if (!searchTextBuf) {
      searchTextBufCapacity = 0;
      LOG_ERR("SCT", "Search failed: OOM for text buffer");
      closeSearchState();
      return {ScanStatus::IoError, -1};
    }
    searchTextBufCapacity = TEXT_BUF_SIZE;
  }
  for (uint16_t i = 0; i < count; i++) {
    uint32_t searchTextOffset = 0;
    memcpy(&searchTextOffset, searchLutBuf.get() + i * PAGE_LUT_ENTRY_SIZE + sizeof(uint32_t), sizeof(uint32_t));
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
      const size_t chunkSize = std::min<size_t>(searchTextBufCapacity, remaining);
      if (file.read(searchTextBuf.get(), chunkSize) != chunkSize) {
        LOG_ERR("SCT", "Search failed: truncated text record");
        closeSearchState();
        return {ScanStatus::IoError, -1};
      }
      remaining -= chunkSize;

      for (size_t j = 0; j < chunkSize; ++j) {
        const int signal = matcher.feed(searchTextBuf[j]);
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
