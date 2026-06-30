#pragma once

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "Epub/Page.h"
#include "ProgressFile.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, sizeof(data))) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

inline bool hasEmSpacePrefix(const std::string& word) {
  return word.size() >= 3 && word.compare(0, 3, "\xe2\x80\x83") == 0;
}

template <typename Callback>
bool forEachVisiblePageWord(const Page& page, Callback&& callback) {
  uint16_t wordIndex = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    if (!line.getBlock()) continue;

    const auto& block = *line.getBlock();
    const auto& wordList = block.getWords();
    const auto& xpos = block.getWordXpos();
    const auto& styles = block.getWordStyles();
    const size_t count = std::min({wordList.size(), xpos.size(), styles.size()});
    for (size_t i = 0; i < count; ++i) {
      const std::string& word = wordList[i];
      const char* visibleWord = word.c_str() + (hasEmSpacePrefix(word) ? 3 : 0);
      bool hasVisibleText = false;
      for (const char* p = visibleWord; *p != '\0'; ++p) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
          hasVisibleText = true;
          break;
        }
      }
      if (!hasVisibleText) continue;

      if (!callback(wordIndex, line, block, i)) {
        return false;
      }
      wordIndex++;
    }
  }
  return true;
}

// Map an inclusive [startByte, endByte] span in a page's serialized search-text
// record (see Page::serializeSearchText) to the inclusive range of visible page
// word indices it covers — the same indices forEachVisiblePageWord and
// drawWordHighlights use. Returns false if the span touches no visible word.
//
// This is the inverse of Page::serializeSearchText: that record is the page's
// words in element/line/block order, each word preceded by a single-byte
// separator except the first word emitted on the page, every word contributing
// its raw word.size() bytes (including any em-space prefix). The walk reproduces
// that byte layout exactly so offsets stay aligned with the matcher's, while
// applying forEachVisiblePageWord's visible-word filter for the returned indices.
// Letting the matcher report byte offsets and mapping them here means the
// highlighter never re-runs search or re-reads the cache to place a match.
inline bool searchByteSpanToWordRange(const Page& page, const uint32_t startByte, const uint32_t endByte,
                                      uint16_t& outFirstWord, uint16_t& outLastWord) {
  uint32_t recordPos = 0;       // byte offset of the next word in the record
  bool emittedAnyWord = false;  // mirrors serializeSearchText's page-wide separator flag
  uint16_t visibleWordIndex = 0;
  bool found = false;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    if (!line.getBlock()) continue;

    const auto& block = *line.getBlock();
    const auto& wordList = block.getWords();
    const auto& xpos = block.getWordXpos();
    const auto& styles = block.getWordStyles();
    // serializeSearchText writes every word in the block; forEachVisiblePageWord
    // only indexes those within this min() guard, so cap the visible-index space
    // the same way while still counting every word's bytes.
    const size_t visibleCount = std::min({wordList.size(), xpos.size(), styles.size()});
    for (size_t i = 0; i < wordList.size(); ++i) {
      const std::string& word = wordList[i];
      if (emittedAnyWord) ++recordPos;  // WORD_SEPARATOR
      emittedAnyWord = true;
      const uint32_t wordStart = recordPos;
      recordPos += static_cast<uint32_t>(word.size());
      const uint32_t wordEnd = recordPos;  // exclusive

      bool visible = i < visibleCount;
      if (visible) {
        const char* vw = word.c_str() + (hasEmSpacePrefix(word) ? 3 : 0);
        bool hasVisibleText = false;
        for (const char* p = vw; *p != '\0'; ++p) {
          if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            hasVisibleText = true;
            break;
          }
        }
        visible = hasVisibleText;
      }
      if (!visible) continue;

      // Inclusive match span overlaps the half-open word byte range.
      if (word.size() > 0 && wordStart <= endByte && wordEnd > startByte) {
        if (!found) {
          outFirstWord = visibleWordIndex;
          found = true;
        }
        outLastWord = visibleWordIndex;
      }
      ++visibleWordIndex;
    }
  }
  return found;
}

// Paint a per-word highlight over every visible word for which `isHighlighted`
// returns true. Owns the shared geometry — em-space prefix offset and the
// next-word width extension that closes the gap between adjacent highlighted
// words — so the search and clipping highlighters cannot drift apart. `drawWord`
// receives the computed rectangle plus the visible text/style and performs the
// fill and text draw in its own style. Templated (not std::function) to stay
// allocation-free on the render path.
template <typename MatchPred, typename DrawWord>
void drawWordHighlights(const Page& page, GfxRenderer& renderer, const int fontId, const int orientedMarginTop,
                        const int orientedMarginLeft, MatchPred&& isHighlighted, DrawWord&& drawWord) {
  forEachVisiblePageWord(
      page, [&](const uint16_t pageWordIndex, const PageLine& line, const TextBlock& block, const size_t i) {
        if (!isHighlighted(pageWordIndex)) {
          return true;
        }

        const auto& wordList = block.getWords();
        const auto& xpos = block.getWordXpos();
        const auto& styles = block.getWordStyles();
        if (i >= wordList.size() || i >= xpos.size() || i >= styles.size()) {
          return true;
        }

        const std::string& wordText = wordList[i];
        const bool hasEmSpace = hasEmSpacePrefix(wordText);
        const char* visibleText = wordText.c_str() + (hasEmSpace ? 3 : 0);
        const auto textStyle = static_cast<EpdFontFamily::Style>(styles[i] & ~EpdFontFamily::UNDERLINE);
        const int skipX = hasEmSpace ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", textStyle) : 0;
        const int wordX = orientedMarginLeft + line.xPos + xpos[i] + skipX;
        const int wordY = orientedMarginTop + line.yPos;
        int wordW = renderer.getTextAdvanceX(fontId, wordText.c_str(), textStyle) - skipX;
        const int wordH = renderer.getLineHeight(fontId);
        if (i + 1 < wordList.size() && i + 1 < xpos.size() && i + 1 < styles.size()) {
          const std::string& nextWordText = wordList[i + 1];
          const bool nextHasEmSpace = hasEmSpacePrefix(nextWordText);
          const auto nextTextStyle = static_cast<EpdFontFamily::Style>(styles[i + 1] & ~EpdFontFamily::UNDERLINE);
          const int nextSkipX = nextHasEmSpace ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", nextTextStyle) : 0;
          const int nextWordX = orientedMarginLeft + line.xPos + xpos[i + 1] + nextSkipX;
          if (isHighlighted(pageWordIndex + 1) && nextWordX > wordX + wordW) {
            wordW = nextWordX - wordX;
          } else if (nextWordX > wordX && wordW > nextWordX - wordX) {
            wordW = nextWordX - wordX;
          }
        }
        if (wordW > 0) {
          drawWord(wordX, wordY, wordW, wordH, visibleText, textStyle);
        }
        return true;
      });
}

}  // namespace EpubReaderUtils
