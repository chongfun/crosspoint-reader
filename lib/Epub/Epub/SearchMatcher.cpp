#include "SearchMatcher.h"

#include <algorithm>
#include <cctype>

#include "AsciiCase.h"

namespace {
// A "word" byte for boundary purposes. Normalization has already folded every
// matchable letter to ASCII a-z, so word characters are exactly [a-z0-9];
// spaces, punctuation, and dropped/unmapped codepoints are boundaries.
bool isWordByte(uint8_t b) { return (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9'); }

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

bool SearchMatcher::queriesEquivalent(const std::string_view a, const std::string_view b) {
  std::array<uint8_t, MAX_QUERY_BYTES> normA{};
  std::array<uint8_t, MAX_QUERY_BYTES> normB{};
  const size_t lenA = normalizeSearchQuery(a, normA);
  const size_t lenB = normalizeSearchQuery(b, normB);
  return lenA == lenB && std::equal(normA.begin(), normA.begin() + lenA, normB.begin());
}

size_t SearchMatcher::normalizeSearchQuery(const std::string_view query, std::array<uint8_t, MAX_QUERY_BYTES>& out) {
  size_t len = 0;
  uint32_t utf8State = 0;
  uint32_t utf8Codepoint = 0;
  bool prevWasHyphen = false;

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

      if (b == '-') {
        // Hyphens are fuzzy: dropped from the query (and the text) so a
        // hyphenated word, including one split across a line, still matches.
        prevWasHyphen = true;
        continue;
      }
      if (b == ' ') {
        // A space right after a hyphen is the word separator that line-break
        // hyphenation leaves between the two halves; drop it so they rejoin.
        // Otherwise a space is a significant word boundary: collapse runs and
        // drop a leading space so the pattern lines up with the text record.
        if (prevWasHyphen) {
          prevWasHyphen = false;
          continue;
        }
        if (len == 0 || out[len - 1] == ' ') {
          continue;
        }
        if (len < out.size()) {
          out[len++] = ' ';
        }
        continue;
      }

      prevWasHyphen = false;
      if (len >= out.size()) {
        break;
      }
      out[len++] = b;
    }
  }
  // Drop a trailing significant space so the pattern is not forced to end on a
  // word boundary that the text record may not provide.
  if (len > 0 && out[len - 1] == ' ') {
    --len;
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

  // A whole-word boundary is only enforced on an edge that is itself a word
  // character, mirroring \b: "cat" requires boundaries on both sides, but a
  // query ending in punctuation does not demand a non-word character after it.
  patternStartsWithWordChar = isWordByte(pattern[0]);
  patternEndsWithWordChar = isWordByte(pattern[length - 1]);

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
    // A codepoint outside the supported fold set (CJK, Cyrillic, etc.) is
    // dropped before matching, exactly like a fuzzy hyphen/space. Carry its raw
    // UTF-8 width so a match that straddles it still spans the full text, e.g.
    // "a你b" matching "ab" highlights all five bytes. Reset the byte counter so
    // the next codepoint's width starts clean.
    if (matched > 0) {
      pendingSeparatorBytes += utf8BytesConsumed;
    }
    utf8BytesConsumed = 0;
    return 0;
  }

  currentCodepointId++;
  const uint16_t ownBytes = utf8BytesConsumed;
  uint16_t currentCodepointWidth = ownBytes + pendingSeparatorBytes;
  utf8BytesConsumed = 0;
  pendingSeparatorBytes = 0;

  int totalWidthReturn = 0;

  for (int shift = 0; shift < 32; shift += 8) {
    uint8_t b = (norm >> shift) & 0xFF;
    if (b == 0) break;

    // Classify the byte as fuzzy (dropped) or a significant character. Hyphens
    // are always dropped. A space is dropped only when it directly follows a
    // hyphen (the separator a line-break hyphenation leaves between the two
    // halves) or another space (run collapse); every other space is significant
    // and must be matched, so a query without a space cannot cross a word
    // boundary. Dropped bytes still extend the match span via pendingSeparatorBytes.
    bool dropAsSeparator = false;
    if (b == '-') {
      prevWasHyphen = true;
      dropAsSeparator = true;
    } else if (b == ' ' && (prevWasHyphen || lastEmittedWasSpace)) {
      prevWasHyphen = false;
      dropAsSeparator = true;
    }
    if (dropAsSeparator) {
      if (matched > 0) {
        pendingSeparatorBytes += currentCodepointWidth;
      }
      continue;
    }

    prevWasHyphen = false;
    lastEmittedWasSpace = (b == ' ');

    // This significant byte is the character immediately after any match that
    // completed earlier, so it decides that match's trailing boundary. A pattern
    // that ends in a word char needs a non-word neighbour here; one ending in a
    // non-word char needs no trailing boundary at all and is confirmed by any
    // following character. (The record's end is handled by the caller.)
    if (pendingActive) {
      pendingActive = false;
      if (!patternEndsWithWordChar || !isWordByte(b)) {
        return -1;  // boundary holds: the pending match is a whole word
      }
      // A word character extends the match into a longer word; reject it and
      // keep scanning. KMP state is untouched, so a later occurrence can match.
    }

    const uint8_t value = b;

    while (matched > 0 && value != pattern[matched]) {
      matched = prefix[matched - 1];
    }

    if (matched == 0) {
      // This codepoint restarts (or never started) a match, so separators
      // accumulated during the previous partial match are not part of this
      // match's span: drop the carried width and count only this codepoint's
      // own bytes. Without this, currentCodepointWidth still includes the stale
      // pendingSeparatorBytes captured above, over-counting the highlight span.
      pendingSeparatorBytes = 0;
      currentCodepointWidth = ownBytes;
    }

    matchByteWidths[widthBufferHead] = currentCodepointWidth;
    matchCodepointIds[widthBufferHead] = currentCodepointId;
    // Record what preceded this byte before updating the running class, so the
    // completion check below can read the character just before the match start.
    precededByWordChar[widthBufferHead] = prevWasWordChar;
    prevWasWordChar = isWordByte(b);
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

        // Leading boundary: the character before the match's first significant
        // byte must be a non-word character (unless the pattern itself starts on
        // a non-word char, where no boundary is required). The start byte sits
        // `length` positions back in the ring, the same index the width sum used.
        bool leadingBoundaryOk = !patternStartsWithWordChar;
        if (!leadingBoundaryOk) {
          const int startIndex = (widthBufferHead + MAX_QUERY_BYTES - length) % MAX_QUERY_BYTES;
          leadingBoundaryOk = !precededByWordChar[startIndex];
        }
        // A multi-character fold (e.g. ß -> "ss") emits more bytes in this same
        // feed, and every such expansion is a run of letters. If the pattern ends
        // on a word char, that next letter is its trailing neighbour and rejects
        // the match here, before the per-feed pending check below would ever see
        // it. (When the pattern ends on a non-word char no trailing boundary is
        // required, so the following byte is harmless.)
        const bool moreBytesInFold = shift + 8 < 32 && ((norm >> (shift + 8)) & 0xFF) != 0;
        if (leadingBoundaryOk && !(patternEndsWithWordChar && moreBytesInFold)) {
          // Tentative: the trailing boundary is confirmed by the next byte (or by
          // the caller at the record's end). Report the width so the caller can
          // record the span; a return here cannot also be a pending confirmation
          // because confirmation returns -1 above before reaching this point.
          totalWidthReturn = totalWidth;
        }
        // A failed leading boundary means the match sits inside a longer word;
        // ignore this completion and let KMP keep scanning for a real one.
      }
    }
  }

  return totalWidthReturn;
}
