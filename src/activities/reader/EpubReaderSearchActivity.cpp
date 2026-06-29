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
// Repaint the progress screen only once the percentage has advanced this much,
// keeping e-ink refreshes bounded now that progress moves per page.
constexpr int PROGRESS_REPAINT_STEP_PERCENT = 1;
}  // namespace

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
      section(this->epub, route.startSpineIndex, renderer,
              ReaderUtils::sectionCacheSuffixForRenderMode(isValidEpubRenderMode(SETTINGS.epubRenderMode)
                                                               ? static_cast<EpubRenderMode>(SETTINGS.epubRenderMode)
                                                               : EpubRenderMode::CrossInkDefault)),
      route(route),
      currentSpineIndex(route.startSpineIndex),
      currentPage(route.startPage),
      viewportWidth(viewportWidth),
      viewportHeight(viewportHeight) {
  bool ok = false;
  if (query) {
    const size_t len = strlen(query);
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
         currentPage == route.stopPage && matcher.matched > 0;
}

void EpubReaderSearchActivity::advanceSpine() {
  ++currentSpineIndex;
  currentPage = 0;
  sectionLoaded = false;
  sectionCacheRepairAttempted = false;
  matcher.reset();  // spine boundary: don't carry a partial match across chapters
}

bool EpubReaderSearchActivity::ensureSectionLoaded() {
  if (sectionLoaded) {
    return true;
  }

  section.resetForSpine(currentSpineIndex);
  const EpubRenderMode renderMode = isValidEpubRenderMode(SETTINGS.epubRenderMode)
                                        ? static_cast<EpubRenderMode>(SETTINGS.epubRenderMode)
                                        : EpubRenderMode::CrossInkDefault;
  if (section.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                              SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                              SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                              SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
                              SETTINGS.guideReadingEnabled, renderMode)) {
    sectionLoaded = true;
    return true;
  }

  LOG_DBG("EPS", "Building section %d for search", currentSpineIndex);
  if (!section.createSectionFile(
          SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
          SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
          SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
          SETTINGS.guideReadingEnabled, nullptr, nullptr, nullptr, renderMode)) {
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

  const size_t matchedBeforeChunk = matcher.matched;
  auto match = section.scanForward(currentPage, endPage, matcher);

  if (match == std::nullopt && !sectionCacheRepairAttempted) {
    sectionCacheRepairAttempted = true;
    matcher.matched = matchedBeforeChunk;

    // Invalidate corrupt cache
    section.resetForSpine(currentSpineIndex);
    sectionLoaded = false;
    section.clearCache();

    if (!ensureSectionLoaded()) {
      return;
    }
    match = section.scanForward(currentPage, endPage, matcher);
  }

  if (match == std::nullopt) {
    // Do not leave a version-valid but unreadable cache to fail every future search.
    section.resetForSpine(currentSpineIndex);
    sectionLoaded = false;
    section.clearCache();
    setFailure(SearchState::Error);
    return;
  }

  if (*match >= 0) {
    setResult(ProgressChangeResult{currentSpineIndex, *match});
    finish();
    return;
  }

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
      // Progress is now page-granular, so only repaint once it has advanced a
      // whole step. This bounds e-ink refreshes to ~100/step over an entire
      // scan regardless of book structure, instead of one per page.
      if (state == SearchState::Searching) {
        const int percent = searchProgressPercent();
        if (percent - lastProgressPercent >= PROGRESS_REPAINT_STEP_PERCENT) {
          lastProgressPercent = percent;
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
