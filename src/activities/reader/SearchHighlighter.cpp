#include "SearchHighlighter.h"

#include <Epub/AsciiCase.h>
#include <Epub/Page.h>
#include <Epub/SearchMatcher.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "EpubReaderUtils.h"

void SearchHighlighter::drawSearchHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                             const int orientedMarginLeft, Section* section,
                                             const char* lastSearchQuery, GfxRenderer& renderer) const {
  if (lastSearchQuery == nullptr || lastSearchQuery[0] == '\0' || !section) {
    return;
  }

  // Recompute the match ranges only when the visible page or query changed.
  // While the reader sits on the search-result page (status-bar refreshes, etc.)
  // this avoids re-reading the previous page from SD and recompiling the matcher
  // on every frame; we just repaint the cached ranges.
  const bool cacheHit = searchHighlightComputed && section->currentPage == searchHighlightCachedPage &&
                        searchHighlightCachedQuery == lastSearchQuery;
  if (!cacheHit) {
    searchHighlightComputed = true;
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
            searchHighlightPageText.push_back((c >= 'A' && c <= 'Z') ? (c + 32) : c);
            searchHighlightCharToWordIndex.push_back(pageWordIndex);
          }
          return true;
        });

    // 3. Find matches of compiledQuery in normalizedPageText incorporating prior page state
    if (section->currentPage > 0) {
      // Prime the matcher with the previous page so a match that began there and
      // completes on this page still highlights. If that scan fails it leaves the
      // matcher mid-feed; reset it so we highlight only matches contained on this
      // page rather than feeding indeterminate carried state.
      const Section::ScanResult primeResult =
          section->scanForward(std::max(0, section->currentPage - 1), section->currentPage, matcher);
      if (primeResult.status == Section::ScanStatus::CorruptCache ||
          primeResult.status == Section::ScanStatus::IoError) {
        matcher.reset();
      }
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
  // the search style: a solid inverted fill (white-on-black) so matches stand out.
  const auto isSearchMatchWord = [this](const uint16_t pageWordIndex) {
    return std::any_of(
        searchHighlightMatchRanges.begin(), searchHighlightMatchRanges.end(),
        [pageWordIndex](const auto& range) { return pageWordIndex >= range.first && pageWordIndex <= range.second; });
  };

  EpubReaderUtils::drawWordHighlights(page, renderer, fontId, orientedMarginTop, orientedMarginLeft, isSearchMatchWord,
                                      [&](const int wordX, const int wordY, const int wordW, const int wordH,
                                          const char* visibleText, const EpdFontFamily::Style textStyle) {
                                        renderer.fillRect(wordX, wordY, wordW, wordH, true);
                                        renderer.drawText(fontId, wordX, wordY, visibleText, false, textStyle);
                                      });
}

void SearchHighlighter::ensureBuffersReserved() const {
  if (searchHighlightPageText.capacity() > 0) {
    return;
  }
  searchHighlightPageText.reserve(4096);
  searchHighlightCharToWordIndex.reserve(4096);
  searchHighlightMatchRanges.reserve(128);
}

void SearchHighlighter::release() {
  searchHighlightPageText.clear();
  searchHighlightPageText.shrink_to_fit();
  searchHighlightCharToWordIndex.clear();
  searchHighlightCharToWordIndex.shrink_to_fit();
  searchHighlightMatchRanges.clear();
  searchHighlightMatchRanges.shrink_to_fit();
  searchHighlightComputed = false;
  searchHighlightCachedPage = -1;
  searchHighlightCachedQuery.clear();
  searchHighlightCachedQuery.shrink_to_fit();
}
