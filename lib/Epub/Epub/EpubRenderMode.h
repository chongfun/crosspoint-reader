#pragma once

#include <cstdint>

enum class EpubRenderMode : uint8_t {
  CrossInkDefault = 0,
  Balanced = 1,
  Light = 2,
};

constexpr uint8_t EPUB_RENDER_MODE_COUNT = 3;

inline bool isValidEpubRenderMode(const uint8_t mode) { return mode < EPUB_RENDER_MODE_COUNT; }

// Coerce a raw stored render-mode byte into a valid EpubRenderMode, falling back
// to the default for out-of-range values. Single definition so every reader of
// SETTINGS.epubRenderMode (the reader and the in-book search activity) folds the
// same way and computes the same section-cache suffix.
inline EpubRenderMode normalizeRenderMode(const uint8_t rawMode) {
  return isValidEpubRenderMode(rawMode) ? static_cast<EpubRenderMode>(rawMode) : EpubRenderMode::CrossInkDefault;
}
