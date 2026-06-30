#pragma once

class GfxRenderer;
class Page;

// Draws the in-book search highlight on the page the search landed on. The match
// location is computed once by the search scan (Section::scanForward) and handed
// down as a byte span; this class is a pure consumer that maps that span to the
// page's words and paints them. It does not run the matcher, normalize text, or
// read the cache, so there is a single matching pipeline (the scan's) rather than
// a second one re-derived at render time.
class SearchHighlighter {
 public:
  // Highlight the matched words on `page`. [matchStartByte, matchEndByte] is the
  // inclusive byte span of the match within the page's serialized search-text
  // record (as reported by Section::scanForward). A negative or inverted span
  // means "nothing to highlight" and draws nothing.
  void drawSearchHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft,
                            int matchStartByte, int matchEndByte, GfxRenderer& renderer) const;
};
