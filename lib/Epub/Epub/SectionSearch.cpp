// Forward text-search scanning over a section's cache file. These Section
// methods live in their own translation unit (rather than Section.cpp) so the
// search feature can evolve without colliding with unrelated edits to the much
// larger section cache writer/reader. They share the on-disk layout constants
// via SectionCacheFormat.h.
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "Section.h"
#include "SectionCacheFormat.h"

#ifdef SEARCH_PROFILE
// Opt-in latency profiling: build firmware with -DSEARCH_PROFILE to log, per
// scanned chunk, how the per-page scan time splits between SD I/O (seek+read)
// and matcher CPU (feed()). Hardware-only: esp_timer is an ESP-IDF facility.
#include <esp_timer.h>
#endif

using namespace epub;

bool Section::ensureSearchHeader(ScanStatus& failureStatus) {
  if (searchScan.headerReady) {
    return true;
  }

  // Open the member file handle lazily on the first call. It stays open for
  // all pages in this section; resetForSpine() closes it when advancing. An
  // open failure is transient I/O, not cache corruption, so a rebuild cannot
  // fix it.
  if (!file) {
    if (!Storage.openFileForRead("SCT", filePath, file)) {
      LOG_ERR("SCT", "Search failed: could not open section cache file");
      failureStatus = ScanStatus::IoError;
      return false;
    }
  }

  const uint32_t fileSize = file.size();
  if (fileSize < HEADER_SIZE) {
    LOG_ERR("SCT", "Search failed: section cache header is truncated");
    // Release the handle so the corrupt cache can be invalidated/rebuilt; the
    // next call reopens lazily (headerReady stays false).
    closeSearchState();
    failureStatus = ScanStatus::CorruptCache;
    return false;
  }

  // The header fits within the validated fileSize, so a seek/read failure here
  // is an I/O problem rather than malformed data.
  uint32_t lutOffset = 0;
  if (!readPageLutOffset(lutOffset)) {
    LOG_ERR("SCT", "Search failed: could not read page LUT offset");
    closeSearchState();
    failureStatus = ScanStatus::IoError;
    return false;
  }

  searchScan.fileSize = fileSize;
  searchScan.lutOffset = lutOffset;
  searchScan.headerReady = true;
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
  // ensureSearchHeader distinguishes a transient I/O failure from a corrupt
  // header so we do not delete a valid cache over a momentary glitch.
  ScanStatus headerFailure = ScanStatus::CorruptCache;
  if (!ensureSearchHeader(headerFailure)) {
    return {headerFailure, -1};
  }
  const uint32_t fileSize = searchScan.fileSize;
  const uint32_t lutOffset = searchScan.lutOffset;

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
  if (searchScan.lutBufCapacity < lutBytes) {
    searchScan.lutBuf = makeUniqueNoThrow<uint8_t[]>(lutBytes);
    if (!searchScan.lutBuf) {
      searchScan.lutBufCapacity = 0;
      LOG_ERR("SCT", "Search failed: OOM for page LUT buffer (%u bytes)", static_cast<unsigned>(lutBytes));
      closeSearchState();
      return {ScanStatus::IoError, -1};
    }
    searchScan.lutBufCapacity = lutBytes;
  }
  if (!file.seek(static_cast<size_t>(entryOffset))) {
    LOG_ERR("SCT", "Search failed: could not seek to page LUT entries");
    closeSearchState();
    return {ScanStatus::IoError, -1};
  }
  if (file.read(searchScan.lutBuf.get(), lutBytes) != lutBytes) {
    // The LUT range was already validated against fileSize above, so a short
    // read here is an I/O failure, not a corrupt cache.
    LOG_ERR("SCT", "Search failed: could not read page LUT entries");
    closeSearchState();
    return {ScanStatus::IoError, -1};
  }

  // Allocate a larger heap buffer to batch text reads, drastically reducing
  // slow SPI transactions over the small 64-byte stack array previously used.
  constexpr size_t TEXT_BUF_SIZE = 2048;
  if (searchScan.textBufCapacity < TEXT_BUF_SIZE) {
    searchScan.textBuf = makeUniqueNoThrow<uint8_t[]>(TEXT_BUF_SIZE);
    if (!searchScan.textBuf) {
      searchScan.textBufCapacity = 0;
      LOG_ERR("SCT", "Search failed: OOM for text buffer");
      closeSearchState();
      return {ScanStatus::IoError, -1};
    }
    searchScan.textBufCapacity = TEXT_BUF_SIZE;
  }

#ifdef SEARCH_PROFILE
  // Per-page scan accounting. profStart covers only the page loop below, not the
  // one-time header/LUT setup, so the split reflects the steady-state cost.
  int64_t profCpuUs = 0;
  uint64_t profBytes = 0;
  const int64_t profStart = esp_timer_get_time();
#endif

  // Sequentially read the text records
  for (uint16_t i = 0; i < count; i++) {
    uint32_t searchTextOffset = 0;
    // searchTextOffset is the 2nd uint32_t in the LUT entry
    memcpy(&searchTextOffset, searchScan.lutBuf.get() + i * PAGE_LUT_ENTRY_SIZE + sizeof(uint32_t), sizeof(uint32_t));
    // Text records (each a u32 length prefix + bytes) live in the page-record
    // region, which starts after the fixed header and ends where the page LUT
    // begins. Bound below by HEADER_SIZE and above by lutOffset (not just
    // fileSize) so a corrupt offset pointing into the header, the LUT, or the
    // trailer is rejected rather than read as text. (lutOffset <= fileSize,
    // validated above.)
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

    // A page with no searchable text (e.g. image-only) is a content discontinuity,
    // so drop any carried partial match rather than bridging across it.
    if (remaining == 0) {
      // The discontinuity is a hard word boundary, so it confirms a match left
      // pending by an earlier page (e.g. one that ended on a line-break hyphen).
      if (matcher.hasPendingMatch()) {
        return {ScanStatus::Match, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
      }
      matcher.reset();
      continue;
    }

    // serializeSearchText writes no separator between pages, so feed an explicit
    // word boundary before each page's content. This keeps page boundaries
    // consistent with in-page word boundaries (a query without a space cannot run
    // two words together across a page break) and lets a page-final line-break
    // hyphen rejoin with the continuation word (the matcher drops a space right
    // after a hyphen). The injected byte is not part of the record, so it is not
    // counted in pageBytePos; it can never complete a match because a compiled
    // query never ends in a space. It can, however, confirm the trailing boundary
    // of a match left pending by the previous page; report it on the page where
    // it completed (the span the matcher carries), not this one.
    if (matcher.feed(' ') < 0) {
      return {ScanStatus::Match, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
    }

    // Byte offset of the next fed byte within this page's text record content
    // (the bytes after the u32 length prefix). Used to report where a completed
    // match lies so the highlighter can map it to words without re-scanning.
    uint32_t pageBytePos = 0;
    while (remaining > 0) {
      const size_t chunkSize = std::min<size_t>(searchScan.textBufCapacity, remaining);
      if (file.read(searchScan.textBuf.get(), chunkSize) != chunkSize) {
        LOG_ERR("SCT", "Search failed: truncated text record");
        closeSearchState();
        return {ScanStatus::IoError, -1};
      }
      remaining -= chunkSize;
#ifdef SEARCH_PROFILE
      profBytes += chunkSize;
      const int64_t profChunkStart = esp_timer_get_time();
#endif

      for (size_t j = 0; j < chunkSize; ++j) {
        const int signal = matcher.feed(searchScan.textBuf[j]);
        if (signal > 0) {
          // A whole-word match completed on textBuf[j], but its trailing boundary
          // is not yet known. Record the span now (textBuf[j] is the match's last
          // byte; it spans the preceding `signal` bytes, clamped to the page
          // start when the match began on an earlier page) and keep feeding so
          // the next byte can confirm or reject it.
          const int endByte = static_cast<int>(pageBytePos);
          const int startByte = (pageBytePos + 1 >= static_cast<uint32_t>(signal))
                                    ? static_cast<int>(pageBytePos + 1 - static_cast<uint32_t>(signal))
                                    : 0;
          matcher.setPendingMatchSpan(static_cast<int>(startPage + i), startByte, endByte);
        } else if (signal < 0) {
          // textBuf[j] is a word boundary that confirms the pending match: report
          // the span captured when it completed.
          return {ScanStatus::Match, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
        }
        ++pageBytePos;
      }
#ifdef SEARCH_PROFILE
      profCpuUs += esp_timer_get_time() - profChunkStart;
#endif
    }

    // The record holds whole, space-separated words with no trailing separator,
    // so its end is a word boundary too — unless a line-break hyphen carries the
    // final word onto the next page. Confirm a match pending on this page's last
    // word here, so it is reported on this page rather than waiting for the next.
    if (matcher.hasPendingMatch() && !matcher.isHyphenPending()) {
      return {ScanStatus::Match, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
    }
  }

#ifdef SEARCH_PROFILE
  {
    const unsigned long totalMs = static_cast<unsigned long>((esp_timer_get_time() - profStart) / 1000);
    const unsigned long cpuMs = static_cast<unsigned long>(profCpuUs / 1000);
    const unsigned long ioMs = totalMs > cpuMs ? totalMs - cpuMs : 0;
    const float kbps = totalMs > 0 ? (static_cast<float>(profBytes) * 1000.0f) / (1024.0f * totalMs) : 0.0f;
    LOG_INF("SCT", "search profile: pages=%u textBytes=%lu total=%lums io(seek+read)=%lums cpu(feed)=%lums (%.1f KB/s)",
            static_cast<unsigned>(count), static_cast<unsigned long>(profBytes), totalMs, ioMs, cpuMs,
            static_cast<double>(kbps));
  }
#endif
  return {ScanStatus::NoMatch, -1};
}
