#include "SearchMatcher.h"

#include <algorithm>
#include <cctype>

#include "AsciiCase.h"

namespace {
constexpr bool isSearchSeparator(const uint8_t b) { return b == ' ' || b == '-'; }

uint32_t stripLatinDiacritics(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 32;

  if (cp >= 0x00E0 && cp <= 0x00E5) return 'a';                  // à á â ã ä å
  if (cp >= 0x00C0 && cp <= 0x00C5) return 'a';                  // À Á Â Ã Ä Å
  if (cp == 0x00E7 || cp == 0x00C7) return 'c';                  // ç Ç
  if (cp >= 0x00E8 && cp <= 0x00EB) return 'e';                  // è é ê ë
  if (cp >= 0x00C8 && cp <= 0x00CB) return 'e';                  // È É Ê Ë
  if (cp >= 0x00EC && cp <= 0x00EF) return 'i';                  // ì í î ï
  if (cp >= 0x00CC && cp <= 0x00CF) return 'i';                  // Ì Í Î Ï
  if (cp == 0x00F1 || cp == 0x00D1) return 'n';                  // ñ Ñ
  if (cp >= 0x00F2 && cp <= 0x00F6) return 'o';                  // ò ó ô õ ö
  if (cp >= 0x00D2 && cp <= 0x00D6) return 'o';                  // Ò Ó Ô Õ Ö
  if (cp >= 0x00F9 && cp <= 0x00FC) return 'u';                  // ù ú û ü
  if (cp >= 0x00D9 && cp <= 0x00DC) return 'u';                  // Ù Ú Û Ü
  if (cp == 0x00FD || cp == 0x00FF || cp == 0x00DD) return 'y';  // ý ÿ Ý

  // Multi-character folding (packed into 32-bit uint)
  if (cp == 0x00DF) return 's' | ('s' << 8);                  // ß -> ss
  if (cp == 0x00E6 || cp == 0x00C6) return 'a' | ('e' << 8);  // æ Æ -> ae
  if (cp == 0x0153 || cp == 0x0152) return 'o' | ('e' << 8);  // œ Œ -> oe
  if (cp == 0xFB00) return 'f' | ('f' << 8);                  // ﬀ -> ff
  if (cp == 0xFB01) return 'f' | ('i' << 8);                  // ﬁ -> fi
  if (cp == 0xFB02) return 'f' | ('l' << 8);                  // ﬂ -> fl
  if (cp == 0xFB03) return 'f' | ('f' << 8) | ('i' << 16);    // ﬃ -> ffi
  if (cp == 0xFB04) return 'f' | ('f' << 8) | ('l' << 16);    // ﬄ -> ffl

  if (cp < 256) return epub::asciiToLower(static_cast<uint8_t>(cp));
  return 0;
}
}  // namespace

bool SearchMatcher::isValidSearchQuery(const std::string_view query) {
  if (query.empty() || query.size() > MAX_QUERY_BYTES) {
    return false;
  }
  // Require at least one byte that survives normalization.
  std::array<uint8_t, MAX_QUERY_BYTES> dummy;
  return normalizeSearchQuery(query, dummy) > 0;
}

size_t SearchMatcher::normalizeSearchQuery(const std::string_view query, std::array<uint8_t, MAX_QUERY_BYTES>& out) {
  size_t len = 0;
  uint32_t utf8State = 0;
  uint32_t utf8Codepoint = 0;

  for (const char ch : query) {
    const uint8_t c = static_cast<uint8_t>(ch);
    if (utf8State == 0) {
      if ((c & 0x80) == 0) {
        utf8Codepoint = c;
      } else if ((c & 0xE0) == 0xC0) {
        utf8Codepoint = c & 0x1F;
        utf8State = 1;
        continue;
      } else if ((c & 0xF0) == 0xE0) {
        utf8Codepoint = c & 0x0F;
        utf8State = 2;
        continue;
      } else if ((c & 0xF8) == 0xF0) {
        utf8Codepoint = c & 0x07;
        utf8State = 3;
        continue;
      } else {
        utf8Codepoint = c;
      }
    } else {
      if ((c & 0xC0) == 0x80) {
        utf8Codepoint = (utf8Codepoint << 6) | (c & 0x3F);
        utf8State--;
        if (utf8State > 0) continue;
      } else {
        utf8State = 0;
        continue;  // drop invalid continuation byte and move on
      }
    }

    uint32_t norm = stripLatinDiacritics(utf8Codepoint);
    if (norm == 0) continue;  // Drop characters that don't normalize to ASCII

    for (int shift = 0; shift < 32; shift += 8) {
      uint8_t b = (norm >> shift) & 0xFF;
      if (b == 0) break;

      if (isSearchSeparator(b)) {
        continue;
      }
      if (len >= out.size()) {
        break;
      }
      out[len++] = b;
    }
  }
  return len;
}

bool SearchMatcher::compile(const std::string_view query) {
  pattern.fill(0);
  prefix.fill(0);
  length = 0;
  reset();

  if (query.empty() || query.size() > MAX_QUERY_BYTES) {
    return false;
  }

  length = normalizeSearchQuery(query, pattern);
  if (length == 0) {
    return false;
  }

  for (size_t i = 1, m = 0; i < length; ++i) {
    const uint8_t value = pattern[i];
    while (m > 0 && value != pattern[m]) {
      m = prefix[m - 1];
    }
    if (value == pattern[m]) {
      ++m;
    }
    prefix[i] = static_cast<uint8_t>(m);
  }
  return true;
}

int SearchMatcher::feed(uint8_t c) {
  if (utf8State == 0) {
    if ((c & 0x80) == 0) {
      utf8Codepoint = c;
      utf8BytesConsumed = 1;
    } else if ((c & 0xE0) == 0xC0) {
      utf8Codepoint = c & 0x1F;
      utf8State = 1;
      utf8BytesConsumed = 1;
      return 0;
    } else if ((c & 0xF0) == 0xE0) {
      utf8Codepoint = c & 0x0F;
      utf8State = 2;
      utf8BytesConsumed = 1;
      return 0;
    } else if ((c & 0xF8) == 0xF0) {
      utf8Codepoint = c & 0x07;
      utf8State = 3;
      utf8BytesConsumed = 1;
      return 0;
    } else {
      utf8Codepoint = c;
      utf8BytesConsumed = 1;
    }
  } else {
    if ((c & 0xC0) == 0x80) {
      utf8Codepoint = (utf8Codepoint << 6) | (c & 0x3F);
      utf8State--;
      utf8BytesConsumed++;
      if (utf8State > 0) return 0;
    } else {
      utf8State = 0;
      utf8BytesConsumed = 0;
      return feed(c);
    }
  }

  uint32_t norm = stripLatinDiacritics(utf8Codepoint);

  if (norm == 0) {
    return 0;
  }

  currentCodepointId++;
  uint8_t currentCodepointWidth = utf8BytesConsumed + pendingSeparatorBytes;
  utf8BytesConsumed = 0;
  pendingSeparatorBytes = 0;

  int totalWidthReturn = 0;

  for (int shift = 0; shift < 32; shift += 8) {
    uint8_t b = (norm >> shift) & 0xFF;
    if (b == 0) break;

    if (isSearchSeparator(b)) {
      if (matched > 0) {
        pendingSeparatorBytes += currentCodepointWidth;
      }
      continue;
    }

    const uint8_t value = b;

    while (matched > 0 && value != pattern[matched]) {
      matched = prefix[matched - 1];
    }

    if (matched == 0) {
      pendingSeparatorBytes = 0;
    }

    matchByteWidths[widthBufferHead] = currentCodepointWidth;
    matchCodepointIds[widthBufferHead] = currentCodepointId;
    widthBufferHead = (widthBufferHead + 1) % MAX_QUERY_BYTES;

    if (value == pattern[matched]) {
      ++matched;
      if (matched == length) {
        int totalWidth = 0;
        uint32_t lastSeenCodepoint = 0;
        for (size_t i = 0; i < length; ++i) {
          int index = (widthBufferHead + MAX_QUERY_BYTES - length + i) % MAX_QUERY_BYTES;
          uint32_t cpId = matchCodepointIds[index];
          if (cpId != lastSeenCodepoint) {
            totalWidth += matchByteWidths[index];
            lastSeenCodepoint = cpId;
          }
        }
        matched = prefix[matched - 1];
        totalWidthReturn = totalWidth;
      }
    }
  }

  return totalWidthReturn;
}
