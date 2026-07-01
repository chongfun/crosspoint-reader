#include "EpubReaderSearchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Minimum wall-clock gap between progress-screen repaints. Each repaint is a
// full-panel FAST_REFRESH that single-core-blocks the scan ~380ms AND disturbs
// the shared SPI bus so the next freshly-opened section's reads run ~5x slower
// (~440ms penalty) — measured on hardware. Refresh cadence therefore dominates
// search latency far more than the scan itself, so we throttle by time rather
// than by percent: this bounds refreshes to ~one per interval no matter how
// large the book or how fast the scan, while still feeling live. Raising it
// trades progress smoothness for less refresh overhead.
constexpr unsigned long PROGRESS_REPAINT_MIN_INTERVAL_MS = 2000;
}  // namespace

EpubReaderSearchActivity::SearchRoute EpubReaderSearchActivity::SearchRoute::plan(const Origin& origin) {
  const int startSpine = (origin.spineIndex >= 0 && origin.spineIndex < origin.spineItemsCount) ? origin.spineIndex : 0;
  // A pending page remap means the section was reflowed but not yet reloaded, so
  // the cached page count belongs to this start spine and must drive the remap.
  const bool hasPendingPageRemap =
      !origin.sectionLoaded && origin.cachedPageCount > 0 && origin.cachedSpineIndex == startSpine;
  // The page the search is initiated from (the wrap normally stops before
  // re-examining it). Only meaningful when the start spine is the reader's spine.
  const int initiatedFromPage = startSpine == origin.spineIndex ? std::max(0, origin.page) : 0;
  // "Find next" only when repeating the same query from the exact previous match
  // and no remap is pending; it then begins one page past the originating page.
  const bool isFindNext = !hasPendingPageRemap && origin.sameQuery &&
                          origin.lastResultSpineIndex == origin.spineIndex && origin.lastResultPage == origin.page;
  return make(startSpine, initiatedFromPage, isFindNext, hasPendingPageRemap ? origin.cachedPageCount : 0);
}

void EpubReaderSearchActivity::SearchRoute::resolvePageCount(const int targetPageCount) {
  if (sourcePageCount <= 0) {
    return;
  }

  const bool findNext = startPage > stopPage;
  if (targetPageCount <= 0) {
    startPage = 0;
    stopPage = 0;
    sourcePageCount = 0;
    return;
  }

  const int64_t remappedPage = static_cast<int64_t>(stopPage) * targetPageCount / sourcePageCount;
  stopPage = std::min(static_cast<int>(remappedPage), targetPageCount - 1);
  startPage = stopPage + (findNext ? 1 : 0);
  sourcePageCount = 0;
}

EpubReaderSearchActivity::EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   const std::shared_ptr<Epub>& epub, const char* query,
                                                   const SearchRoute& route, const uint16_t viewportWidth,
                                                   const uint16_t viewportHeight)
    : Activity("EpubReaderSearch", renderer, mappedInput),
      epub(epub),
      section(this->epub, route.startSpineIndex, renderer),
      route(route),
      currentSpineIndex(route.startSpineIndex),
      currentPage(route.startPage),
      viewportWidth(viewportWidth),
      viewportHeight(viewportHeight) {
  bool ok = false;
  if (query) {
    const size_t len = strnlen(query, SearchMatcher::MAX_QUERY_BYTES + 1);
    if (len <= SearchMatcher::MAX_QUERY_BYTES) {
      strncpy(this->query.data(), query, this->query.size() - 1);
      this->query[this->query.size() - 1] = '\0';
      if (matcher.compile(this->query.data())) {
        ok = true;
      }
    }
  }
  // Compile the query once here; every page scan reuses the pattern + table. A
  // rejected query (empty/oversized, or only separators) means there is nothing
  // to scan, so fail closed rather than relying solely on the caller's gate.
  if (!ok) {
    state = SearchState::NotFound;
  }
}

void EpubReaderSearchActivity::onEnter() {
  Activity::onEnter();
  // Paint the status screen before an uncached chapter starts its potentially
  // long layout pass.
  requestUpdateAndWait();
  // Seed the repaint throttle from this initial 0% paint so the first nonzero
  // percent waits a full interval instead of firing an immediate second refresh
  // (lastProgressRepaintMs starts at 0, which the time gate would treat as long
  // overdue).
  lastProgressRepaintMs = millis();
}

void EpubReaderSearchActivity::onExit() { Activity::onExit(); }

bool EpubReaderSearchActivity::skipLoopDelay() { return state == SearchState::Searching; }

bool EpubReaderSearchActivity::preventAutoSleep() { return state == SearchState::Searching; }

void EpubReaderSearchActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderSearchActivity::setFailure(const SearchState failureState) {
  state = failureState;
  requestUpdate();
}

bool EpubReaderSearchActivity::reachedWrappedStop() const {
  if (!wrapped) {
    return false;
  }
  return currentSpineIndex > route.startSpineIndex ||
         (currentSpineIndex == route.startSpineIndex && currentPage >= route.stopPage);
}

bool EpubReaderSearchActivity::shouldScanWrappedStopContinuation() const {
  // A fresh search already scanned stopPage from a clean KMP state. Revisit it
  // only when the preceding page left a partial match; this admits the one
  // occurrence that crosses the circular route boundary without changing find
  // next's originating-page exclusion.
  return wrapped && route.startPage == route.stopPage && currentSpineIndex == route.startSpineIndex &&
         currentPage == route.stopPage && matcher.hasPartialMatch();
}

void EpubReaderSearchActivity::advanceSpine() {
  ++currentSpineIndex;
  currentPage = 0;
  sectionLoaded = false;
  sectionCacheRepairAttempted = false;
  matcher.reset();  // spine boundary: don't carry a partial match across chapters
}

void EpubReaderSearchActivity::dropSectionCache() {
  section.resetForSpine(currentSpineIndex);
  sectionLoaded = false;
  section.clearCache();
}

bool EpubReaderSearchActivity::ensureSectionLoaded() {
  if (sectionLoaded) {
    return true;
  }

  section.resetForSpine(currentSpineIndex);
  if (section.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                              SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                              viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                              SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    sectionLoaded = true;
    return true;
  }

  LOG_DBG("EPS", "Building section %d for search", currentSpineIndex);
  if (!section.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                 SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                 viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                 SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    LOG_ERR("EPS", "Failed to build section %d for search", currentSpineIndex);
    setFailure(SearchState::Error);
    return false;
  }

  sectionLoaded = true;
  return true;
}

bool EpubReaderSearchActivity::advanceSpineIfNeeded() {
  const int spineCount = epub ? epub->getSpineItemsCount() : 0;
  if (spineCount <= 0) {
    setFailure(SearchState::Error);
    return false;
  }

  while (true) {
    if (currentSpineIndex >= spineCount) {
      if (wrapped) {
        setFailure(SearchState::NotFound);
        return false;
      }
      wrapped = true;
      currentSpineIndex = 0;
      currentPage = 0;
      sectionLoaded = false;
      sectionCacheRepairAttempted = false;
      matcher.reset();  // wrap is not contiguous reading text
    }

    if (reachedWrappedStop() && !shouldScanWrappedStopContinuation()) {
      setFailure(SearchState::NotFound);
      return false;
    }

    if (!ensureSectionLoaded()) {
      return false;  // ensureSectionLoaded calls setFailure
    }

    if (!wrapped && currentSpineIndex == route.startSpineIndex && route.sourcePageCount > 0) {
      route.resolvePageCount(section.pageCount);
      currentPage = route.startPage;
    }

    if (scanStartPos < 0.0f && !wrapped && currentSpineIndex == route.startSpineIndex && epub) {
      const float pc = section.pageCount > 0 ? static_cast<float>(section.pageCount) : 1.0f;
      scanStartPos =
          epub->calculateProgress(route.startSpineIndex, std::min(1.0f, static_cast<float>(route.startPage) / pc));
      const float stopPos =
          epub->calculateProgress(route.startSpineIndex, std::min(1.0f, static_cast<float>(route.stopPage) / pc));
      scanRouteLength = (1.0f - scanStartPos) + stopPos;
    }

    if (currentPage >= 0 && currentPage < section.pageCount) {
      return true;
    }

    advanceSpine();
  }
}

void EpubReaderSearchActivity::scanNextPage() {
  if (!advanceSpineIfNeeded()) {
    return;
  }

  int endPage = section.pageCount;
  if (wrapped && currentSpineIndex == route.startSpineIndex) {
    endPage = route.stopPage;
    if (shouldScanWrappedStopContinuation()) {
      endPage = route.stopPage + 1;  // allow scanning the exact stopPage to finish a carried match
    }
  }

  // Chunk scan to 50 pages at a time to yield to the main render/input loop
  endPage = std::min<int>(endPage, currentPage + 50);

  matcherBeforeChunk = matcher;
  auto result = section.scanForward(currentPage, endPage, matcher);

  // A transient I/O failure or OOM is not corruption: surface the error without
  // deleting a valid cache or forcing a re-layout that would just fail again.
  if (result.status == Section::ScanStatus::IoError) {
    setFailure(SearchState::Error);
    return;
  }

  // A structurally corrupt cache can sometimes be repaired by rebuilding once.
  if (result.status == Section::ScanStatus::CorruptCache && !sectionCacheRepairAttempted) {
    sectionCacheRepairAttempted = true;
    matcher = matcherBeforeChunk;

    dropSectionCache();

    if (!ensureSectionLoaded()) {
      return;
    }
    result = section.scanForward(currentPage, endPage, matcher);
    if (result.status == Section::ScanStatus::IoError) {
      setFailure(SearchState::Error);
      return;
    }
  }

  if (result.status == Section::ScanStatus::CorruptCache) {
    // Still corrupt after a rebuild: drop the bad cache and surface the error
    // rather than entering an unbounded rebuild loop.
    dropSectionCache();
    setFailure(SearchState::Error);
    return;
  }

  if (result.status == Section::ScanStatus::Match) {
    setResult(ProgressChangeResult{currentSpineIndex, result.page, result.matchStartByte, result.matchEndByte});
    finish();
    return;
  }

  // NoMatch: advance past the scanned chunk.
  currentPage = endPage;
}

int EpubReaderSearchActivity::searchProgressPercent() const {
  if (!epub || scanStartPos < 0.0f) {
    return 0;  // not yet started / start spine not loaded
  }
  if (scanRouteLength <= 0.0f) {
    return 100;  // degenerate route (nothing eligible to scan)
  }
  // Current byte-weighted book position, using the same model as the reader's
  // own progress bar (so the percentage tracks the bar rather than approximating
  // every spine as equal length).
  const float pc = section.pageCount > 0 ? static_cast<float>(section.pageCount) : 1.0f;
  const float posNow = epub->calculateProgress(currentSpineIndex, std::min(1.0f, static_cast<float>(currentPage) / pc));
  // Work done since the scan began: forward distance before the wrap, plus a
  // full forward lap (1.0 - start) once wrapped.
  const float workDone = wrapped ? (1.0f - scanStartPos) + posNow : posNow - scanStartPos;
  return ReaderUtils::clampPercent(static_cast<int>((workDone / scanRouteLength) * 100.0f + 0.5f));
}

void EpubReaderSearchActivity::loop() {
  // Do NOT poll input here. main.cpp's loop() already calls gpio.update() once
  // per iteration before dispatching to this activity; a second poll would clear
  // the just-latched press/release events (InputManager::update zeroes them every
  // call) before wasReleased() reads them, making Back/Confirm undismissable.
  switch (state) {
    case SearchState::Searching:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        cancel();
        return;
      }
      scanNextPage();
      // Repaint the progress percentage at most once per interval. Each e-ink
      // refresh is expensive (see PROGRESS_REPAINT_MIN_INTERVAL_MS), so we gate
      // on both a changed percentage and elapsed wall-clock rather than per page
      // or per percent — keeping refreshes (and the scan stalls they cause) rare
      // no matter the book size.
      if (state == SearchState::Searching) {
        const int percent = searchProgressPercent();
        const unsigned long now = millis();
        if (percent != lastProgressPercent && now - lastProgressRepaintMs >= PROGRESS_REPAINT_MIN_INTERVAL_MS) {
          lastProgressPercent = percent;
          lastProgressRepaintMs = now;
          requestUpdate();
        }
      }
      return;

    case SearchState::NotFound:
    case SearchState::Error:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        cancel();
      }
      return;
  }
}

void EpubReaderSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SEARCH));
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      query.data());

  const char* message = nullptr;
  switch (state) {
    case SearchState::Searching:
      message = tr(STR_SEARCHING_BOOK);
      break;
    case SearchState::NotFound:
      message = tr(STR_NO_SEARCH_RESULTS);
      break;
    case SearchState::Error:
      message = tr(STR_ERROR_GENERAL_FAILURE);
      break;
  }

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight;
  const int messageY = contentTop + (screen.height - contentTop) / 2;
  UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, messageY, message, true, EpdFontFamily::BOLD);

  if (state == SearchState::Searching) {
    // Draw the value loop() already computed and gated the repaint on, rather
    // than recomputing the progress here.
    char percentText[8];
    snprintf(percentText, sizeof(percentText), "%d%%", lastProgressPercent);
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, messageY + renderer.getLineHeight(UI_12_FONT_ID),
                              percentText, true);
  }

  // While searching, Back cancels. On terminal states (NotFound/Error) both Back
  // and Confirm dismiss to the reader (see loop()), so advertise both.
  const bool terminal = state != SearchState::Searching;
  const char* backLabel = terminal ? tr(STR_BACK) : tr(STR_CANCEL);
  const char* confirmLabel = terminal ? tr(STR_DONE) : "";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(terminal ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}
