#include "SearchHighlighter.h"

#include <Epub/AsciiCase.h>
#include <Epub/Page.h>
#include <Epub/SearchMatcher.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "EpubReaderUtils.h"
#include "ReaderUtils.h"

void SearchHighlighter::drawSearchHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                             const int orientedMarginLeft, Section* section,
                                             const int currentSpineIndex, const char* lastSearchQuery,
                                             GfxRenderer& renderer) const {
  if (lastSearchQuery == nullptr || lastSearchQuery[0] == '\0' || !section) {
    return;
  }

  // Recompute the match ranges only when the visible page or query changed.
  // While the reader sits on the search-result page (status-bar refreshes, etc.)
  // this avoids re-reading the previous page from SD and recompiling the matcher
  // on every frame; we just repaint the cached ranges.
  const bool cacheHit = searchHighlightComputed && searchHighlightCachedSpine == currentSpineIndex &&
                        section->currentPage == searchHighlightCachedPage &&
                        searchHighlightCachedQuery == lastSearchQuery;
  if (!cacheHit) {
    searchHighlightComputed = true;
    searchHighlightCachedSpine = currentSpineIndex;
    searchHighlightCachedPage = section->currentPage;
    searchHighlightCachedQuery = lastSearchQuery;
    searchHighlightMatchRanges.clear();

    // 1. Compile the search query once using KMP
    SearchMatcher matcher;
    if (!matcher.compile(lastSearchQuery)) {
      return;
    }

    ensureBuffersReserved();

    // 2. Normalize the page text and map characters to word indices
    searchHighlightPageText.clear();
    searchHighlightCharToWordIndex.clear();

    EpubReaderUtils::forEachVisiblePageWord(
        page, [&](const uint16_t pageWordIndex, const PageLine& line, const TextBlock& block, const size_t i) {
          const std::string& wordText = block.getWords()[i];
          for (char c : wordText) {
            if (epub::isSearchSeparator(static_cast<uint8_t>(c))) {
              continue;
            }
            if (searchHighlightPageText.size() >= searchHighlightPageText.capacity() ||
                searchHighlightCharToWordIndex.size() >= searchHighlightCharToWordIndex.capacity()) {
              return false;
            }
            searchHighlightPageText.push_back(static_cast<char>(epub::asciiToLower(static_cast<uint8_t>(c))));
            searchHighlightCharToWordIndex.push_back(pageWordIndex);
          }
          return true;
        });

    // 3. Find matches of compiledQuery in normalizedPageText incorporating prior page state
    if (section->currentPage > 0) {
      // Prime the matcher with the previous page so a match that began there and
      // completes on this page still highlights. Only NoMatch means we fed the
      // entire previous page cleanly and the carried partial state is valid at the
      // page boundary. A Match means scanForward stopped mid-previous-page (its
      // carried KMP state is not the boundary state and would mis-highlight this
      // page); CorruptCache/IoError leave the feed indeterminate. Reset in all of
      // those cases so we highlight only matches contained on this page.
      const Section::ScanResult primeResult =
          section->scanForward(std::max(0, section->currentPage - 1), section->currentPage, matcher);
      if (primeResult.status != Section::ScanStatus::NoMatch) {
        matcher.reset();
      }
      // scanForward leaves the section's cache file open on success; the rest of
      // this function only reads in-memory buffers, so release the handle now and
      // do not leave the reader's live section holding an open SD file while it
      // sits on the result page.
      section->closeSearchState();
    }

    for (size_t charIndex = 0; charIndex < searchHighlightPageText.size(); ++charIndex) {
      int matchBytes = matcher.feed(searchHighlightPageText[charIndex]);
      if (matchBytes > 0) {
        size_t startIdx = (charIndex + 1 >= static_cast<size_t>(matchBytes)) ? (charIndex + 1 - matchBytes) : 0;
        size_t endIdx = charIndex;
        if (startIdx < searchHighlightCharToWordIndex.size() && endIdx < searchHighlightCharToWordIndex.size()) {
          if (searchHighlightMatchRanges.size() < searchHighlightMatchRanges.capacity()) {
            searchHighlightMatchRanges.push_back(
                {searchHighlightCharToWordIndex[startIdx], searchHighlightCharToWordIndex[endIdx]});
          }
        }
      }
    }
  }

  if (searchHighlightMatchRanges.empty()) {
    return;
  }

  // 4. Highlight matched words on page using the shared geometry helper, with
  // the search style: a solid inverted fill so matches stand out. "Inverted"
  // means foreground-on-background swapped relative to body text, so it must
  // track the theme: black fill + white text in light mode, white fill + black
  // text in dark mode. Hard-coding black/white made the highlight vanish in dark
  // mode (black fill on a black page, white text identical to body text).
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  const auto isSearchMatchWord = [this](const uint16_t pageWordIndex) {
    return std::any_of(
        searchHighlightMatchRanges.begin(), searchHighlightMatchRanges.end(),
        [pageWordIndex](const auto& range) { return pageWordIndex >= range.first && pageWordIndex <= range.second; });
  };

  EpubReaderUtils::drawWordHighlights(page, renderer, fontId, orientedMarginTop, orientedMarginLeft, isSearchMatchWord,
                                      [&](const int wordX, const int wordY, const int wordW, const int wordH,
                                          const char* visibleText, const EpdFontFamily::Style textStyle) {
                                        renderer.fillRect(wordX, wordY, wordW, wordH, foregroundBlack);
                                        renderer.drawText(fontId, wordX, wordY, visibleText, !foregroundBlack,
                                                          textStyle);
                                      });
}

void SearchHighlighter::ensureBuffersReserved() const {
  constexpr size_t PAGE_TEXT_CAPACITY = 4096;
  constexpr size_t MATCH_RANGES_CAPACITY = 128;
  // Gate on the char->word vector, not searchHighlightPageText: the latter is a
  // std::string whose capacity() is never 0 (small-string optimization keeps ~15
  // bytes inline), so using it as the "already reserved" sentinel would skip the
  // reserve on the first call, leaving the vectors at capacity 0 and tripping the
  // build loop's capacity guard on the very first character (empty page text ->
  // no highlight). The vector's capacity is genuinely 0 until reserved.
  if (searchHighlightCharToWordIndex.capacity() >= PAGE_TEXT_CAPACITY) {
    return;
  }
  searchHighlightPageText.reserve(PAGE_TEXT_CAPACITY);
  searchHighlightCharToWordIndex.reserve(PAGE_TEXT_CAPACITY);
  searchHighlightMatchRanges.reserve(MATCH_RANGES_CAPACITY);
}

void SearchHighlighter::release() {
  searchHighlightPageText.clear();
  searchHighlightPageText.shrink_to_fit();
  searchHighlightCharToWordIndex.clear();
  searchHighlightCharToWordIndex.shrink_to_fit();
  searchHighlightMatchRanges.clear();
  searchHighlightMatchRanges.shrink_to_fit();
  searchHighlightComputed = false;
  searchHighlightCachedSpine = -1;
  searchHighlightCachedPage = -1;
  searchHighlightCachedQuery.clear();
  searchHighlightCachedQuery.shrink_to_fit();
}
