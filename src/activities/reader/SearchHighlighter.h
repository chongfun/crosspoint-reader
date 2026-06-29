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
  SearchHighlighter() = default;

  void drawSearchHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                            const int orientedMarginLeft, Section* section, const char* lastSearchQuery,
                            GfxRenderer& renderer) const;

  // Free the scratch buffers (~12 KB). Called when the on-page highlight is no
  // longer active so a reader who is not viewing a search result does not hold
  // the footprint; drawSearchHighlights re-reserves lazily on the next use.
  void release();

 private:
  // Reserve the scratch buffers on first use; no-op once reserved.
  void ensureBuffersReserved() const;

  mutable std::string searchHighlightPageText;
  mutable std::vector<uint16_t> searchHighlightCharToWordIndex;
  mutable std::vector<std::pair<uint16_t, uint16_t>> searchHighlightMatchRanges;
};
