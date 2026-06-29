#include "SearchHighlighter.h"

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

  // 1. Compile the search query once using KMP
  SearchMatcher matcher;
  if (!matcher.compile(lastSearchQuery)) {
    return;
  }

  // 2. Normalize the page text and map characters to word indices
  searchHighlightPageText.clear();
  searchHighlightCharToWordIndex.clear();

  EpubReaderUtils::forEachVisiblePageWord(
      page, [&](const uint16_t pageWordIndex, const PageLine& line, const TextBlock& block, const size_t i) {
        const std::string& wordText = block.getWords()[i];
        for (char c : wordText) {
          if (c == ' ' || c == '-') {
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
  searchHighlightMatchRanges.clear();
  if (section->currentPage > 0) {
    section->scanForward(std::max(0, section->currentPage - 1), section->currentPage, matcher);
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

  if (searchHighlightMatchRanges.empty()) {
    return;
  }

  // 4. Highlight matched words on page
  const auto isSearchMatchWord = [this](const uint16_t pageWordIndex) {
    return std::any_of(
        searchHighlightMatchRanges.begin(), searchHighlightMatchRanges.end(),
        [pageWordIndex](const auto& range) { return pageWordIndex >= range.first && pageWordIndex <= range.second; });
  };

  EpubReaderUtils::forEachVisiblePageWord(
      page, [&](const uint16_t pageWordIndex, const PageLine& line, const TextBlock& block, const size_t i) {
        if (!isSearchMatchWord(pageWordIndex)) {
          return true;
        }

        const auto& wordList = block.getWords();
        const auto& xpos = block.getWordXpos();
        const auto& styles = block.getWordStyles();
        if (i >= wordList.size() || i >= xpos.size() || i >= styles.size()) {
          return true;
        }

        const std::string& wordText = wordList[i];
        const bool hasEmSpace = EpubReaderUtils::hasEmSpacePrefix(wordText);
        const char* visibleText = wordText.c_str() + (hasEmSpace ? 3 : 0);
        const auto textStyle = static_cast<EpdFontFamily::Style>(styles[i] & ~EpdFontFamily::UNDERLINE);
        const int skipX = hasEmSpace ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", textStyle) : 0;
        const int wordX = orientedMarginLeft + line.xPos + xpos[i] + skipX;
        const int wordY = orientedMarginTop + line.yPos;
        int wordW = renderer.getTextAdvanceX(fontId, wordText.c_str(), textStyle) - skipX;
        const int wordH = renderer.getLineHeight(fontId);
        if (i + 1 < wordList.size() && i + 1 < xpos.size() && i + 1 < styles.size()) {
          const std::string& nextWordText = wordList[i + 1];
          const bool nextHasEmSpace = EpubReaderUtils::hasEmSpacePrefix(nextWordText);
          const auto nextTextStyle = static_cast<EpdFontFamily::Style>(styles[i + 1] & ~EpdFontFamily::UNDERLINE);
          const int nextSkipX = nextHasEmSpace ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", nextTextStyle) : 0;
          const int nextWordX = orientedMarginLeft + line.xPos + xpos[i + 1] + nextSkipX;
          if (isSearchMatchWord(pageWordIndex + 1) && nextWordX > wordX + wordW) {
            wordW = nextWordX - wordX;
          } else if (nextWordX > wordX && wordW > nextWordX - wordX) {
            wordW = nextWordX - wordX;
          }
        }
        if (wordW > 0) {
          renderer.fillRect(wordX, wordY, wordW, wordH, true);
          renderer.drawText(fontId, wordX, wordY, visibleText, false, textStyle);
        }
        return true;
      });
}
