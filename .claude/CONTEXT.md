# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## Simulator

- Simulator patches belong in the adjacent `crosspoint-simulator` repo.
- The valid local simulator env in this repo is `simulator`, and `pio run -e simulator` currently builds cleanly.
- The simulator `PNGdec` stub in `crosspoint-simulator/src/PNGdec.h` needs to mirror the real API shape used by app code, including `hasAlpha()` and `getTransparentColor()`, even though decode still fails intentionally.
- Known simulator limits:
  - No image rendering: `platformio.ini` ignores `hal`, `PNGdec`, and `JPEGDEC`, so image decoders are intentionally absent.
  - JPEGDEC stub always fails; `JPEGDEC fallback: open failed (err=-1)` is expected in simulator.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## Input / Assignable Actions

- Two action enums back the four assignable button shortcuts: `SHORT_PWRBTN` drives `shortPwrBtn`/`longPwrBtn` (power short/long press), `LONG_PRESS_MENU_ACTION` drives `longPressMenuAction`/`longPressBackAction`.
- Power-button actions are dispatched globally in `main.cpp` (`handleGlobalPowerButtonAction` before `activityManager.loop()`), gated by `isPowerButtonActionAvailableOutsideReader()` in `GlobalActions.h`. Long-press Menu/Back actions are otherwise dispatched only inside the reading view via `EpubReaderActivity::executeReaderQuickAction()`; they do not fire from settings/options screens.
- `Quick Return` (`QUICK_RETURN` / `LONG_MENU_QUICK_RETURN`) is the exception: a dedicated long-press Back/Menu interceptor in the main loop (`handleQuickReturnLongPress`, gated by `ActivityManager::quickReturnHasTarget()`) lets it also fire from nested settings. `ActivityManager::quickReturn()` reveals a live reader parked on the stack (calling `Activity::onReveal()` to re-apply orientation/layout) or else `goHome()`.
- Settings leaf screens that exit on Back **press** (`finishAfterBackPress()`: StatusBarSettings, KOReaderSettings, KOReaderAuth, FontDownload) pop one level before a long-press matures, so long-press-Back Quick Return shows a brief intermediate step there. Power-button Quick Return is unaffected.

## Misc Repo Gotchas

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.
