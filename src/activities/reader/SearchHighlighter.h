#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class GfxRenderer;
class Page;
class Section;

class SearchHighlighter {
 public:
  SearchHighlighter() {
    searchHighlightPageText.reserve(4096);
    searchHighlightCharToWordIndex.reserve(4096);
    searchHighlightMatchRanges.reserve(128);
  }

  void drawSearchHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                            const int orientedMarginLeft, Section* section, const char* lastSearchQuery,
                            GfxRenderer& renderer) const;

 private:
  mutable std::string searchHighlightPageText;
  mutable std::vector<uint16_t> searchHighlightCharToWordIndex;
  mutable std::vector<std::pair<uint16_t, uint16_t>> searchHighlightMatchRanges;
};
