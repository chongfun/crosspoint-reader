#include <HalStorage.h>
#include <Logging.h>

#include "Page.h"

bool Page::serializeSearchText(HalFile& file) const {
  // Single pass: write a placeholder length, stream the words while counting
  // the bytes emitted, then seek back and back-patch the real length. Keeping
  // one walk of the elements/words means the recorded length can never diverge
  // from the bytes actually written (the file is O_RDWR and seekable). A page
  // cannot hold anywhere near 4 GB of text, so a uint32 byte count cannot wrap.
  const uint32_t lengthPos = file.position();
  uint32_t textLength = 0;
  if (file.write(reinterpret_cast<const uint8_t*>(&textLength), sizeof(textLength)) != sizeof(textLength)) {
    LOG_ERR("PGE", "Failed to write search text length");
    return false;
  }

  bool hasWord = false;
  static constexpr uint8_t WORD_SEPARATOR = ' ';
  for (const auto& element : elements) {
    if (element->getTag() != TAG_PageLine) {
      continue;
    }

    const auto& line = static_cast<const PageLine&>(*element);
    if (!line.getBlock()) {
      continue;
    }

    for (const auto& word : line.getBlock()->getWords()) {
      if (hasWord) {
        if (file.write(&WORD_SEPARATOR, sizeof(WORD_SEPARATOR)) != sizeof(WORD_SEPARATOR)) {
          LOG_ERR("PGE", "Failed to write search text separator");
          return false;
        }
        ++textLength;
      }
      if (!word.empty()) {
        if (file.write(reinterpret_cast<const uint8_t*>(word.data()), word.size()) != word.size()) {
          LOG_ERR("PGE", "Failed to write search text word");
          return false;
        }
        textLength += static_cast<uint32_t>(word.size());
      }
      hasWord = true;
    }
  }

  const uint32_t endPos = file.position();
  if (!file.seek(lengthPos)) {
    LOG_ERR("PGE", "Failed to seek for search text length back-patch");
    return false;
  }
  if (file.write(reinterpret_cast<const uint8_t*>(&textLength), sizeof(textLength)) != sizeof(textLength)) {
    LOG_ERR("PGE", "Failed to back-patch search text length");
    return false;
  }
  // Restore the write position to the record end so the next page appends
  // correctly; failing this would corrupt the following page's data.
  if (!file.seek(endPos)) {
    LOG_ERR("PGE", "Failed to restore position after search text back-patch");
    return false;
  }
  return true;
}
