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

  // Sequentially read the text records
  std::array<uint8_t, 64> buffer;
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
    if (file.read(reinterpret_cast<uint8_t*>(&remaining), sizeof(remaining)) != sizeof(remaining) ||
        remaining > lutOffset - searchTextOffset - sizeof(uint32_t)) {
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
