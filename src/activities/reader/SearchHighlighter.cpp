#include "SearchHighlighter.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "EpubReaderUtils.h"
#include "ReaderUtils.h"

void SearchHighlighter::drawSearchHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                             const int orientedMarginLeft, const int matchStartByte,
                                             const int matchEndByte, GfxRenderer& renderer) const {
  if (matchStartByte < 0 || matchEndByte < matchStartByte) {
    return;
  }

  // The search scan already located the match; map its byte span to the page's
  // word indices (the inverse of Page::serializeSearchText) rather than
  // re-matching the text or re-reading the previous page from SD.
  uint16_t firstWord = 0;
  uint16_t lastWord = 0;
  if (!EpubReaderUtils::searchByteSpanToWordRange(page, static_cast<uint32_t>(matchStartByte),
                                                  static_cast<uint32_t>(matchEndByte), firstWord, lastWord)) {
    return;
  }

  // Highlight the matched words using the shared geometry helper, with the search
  // style: a solid inverted fill so matches stand out. "Inverted" means
  // foreground-on-background swapped relative to body text, so it must track the
  // theme: black fill + white text in light mode, white fill + black text in dark
  // mode. Hard-coding black/white made the highlight vanish in dark mode (black
  // fill on a black page, white text identical to body text).
  const bool foregroundBlack = true;
  const auto isSearchMatchWord = [firstWord, lastWord](const uint16_t pageWordIndex) {
    return pageWordIndex >= firstWord && pageWordIndex <= lastWord;
  };

  EpubReaderUtils::drawWordHighlights(page, renderer, fontId, orientedMarginTop, orientedMarginLeft, isSearchMatchWord,
                                      [&](const int wordX, const int wordY, const int wordW, const int wordH,
                                          const char* visibleText, const EpdFontFamily::Style textStyle) {
                                        renderer.fillRect(wordX, wordY, wordW, wordH, foregroundBlack);
                                        renderer.drawText(fontId, wordX, wordY, visibleText, !foregroundBlack,
                                                          textStyle);
                                      });
}
