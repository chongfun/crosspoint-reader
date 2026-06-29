#pragma once

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "Epub/Page.h"

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
};

inline bool readProgressFile(const char* moduleName, const std::string& path, Progress& progress) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead(moduleName, path, f)) {
    return false;
  }

  uint8_t data[6];
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize != 4 && dataSize != 6) {
    LOG_ERR(moduleName, "Progress file has unexpected size: %d", dataSize);
    return false;
  }

  progress.spineIndex = static_cast<int>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
  progress.pageNumber = static_cast<int>(static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8));
  if (progress.pageNumber == UINT16_MAX) {
    progress.pageNumber = 0;
  }
  if (dataSize == 6) {
    progress.pageCount = static_cast<int>(static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8));
    progress.hasPageCount = true;
  } else {
    progress.pageCount = 0;
    progress.hasPageCount = false;
  }
  return true;
}

inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  if (readProgressFile(moduleName, progressPath, progress)) {
    return true;
  }

  const std::string backupPath = progressPath + ".bak";
  if (readProgressFile(moduleName, backupPath, progress)) {
    LOG_DBG("ERS", "Recovered progress from backup");
    return true;
  }
  return false;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  const std::string tmpPath = progressPath + ".tmp";
  const std::string backupPath = progressPath + ".bak";

  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR("ERS", "Could not remove stale progress temp file");
    return false;
  }

  FsFile f;
  if (!Storage.openFileForWrite("ERS", tmpPath, f)) {
    LOG_ERR("ERS", "Could not open progress temp file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  f.flush();
  if (!f.sync()) {
    LOG_ERR("ERS", "Failed to sync progress temp file");
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!f.close()) {
    LOG_ERR("ERS", "Failed to close progress temp file");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("ERS", "Could not remove old progress backup");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.exists(progressPath.c_str()) && !Storage.rename(progressPath.c_str(), backupPath.c_str())) {
    LOG_ERR("ERS", "Could not rotate progress backup");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), progressPath.c_str())) {
    LOG_ERR("ERS", "Could not replace progress file");
    if (Storage.exists(backupPath.c_str()) && !Storage.exists(progressPath.c_str())) {
      Storage.rename(backupPath.c_str(), progressPath.c_str());
    }
    Storage.remove(tmpPath.c_str());
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
