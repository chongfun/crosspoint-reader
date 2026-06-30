#pragma once

#include <array>
#include <cstdint>
#include <string_view>

class SearchMatcher {
 public:
  static constexpr size_t MAX_QUERY_BYTES = 64;

  // Single source of truth for whether a query is usable for search: non-empty,
  // not all-whitespace, and within the byte limit. The UI validates with this
  // before launching a search.
  static bool isValidSearchQuery(std::string_view query);

  // True when two queries normalize to the same byte sequence under the exact
  // folding compile() applies (case, Latin diacritics, hyphen/space fuzzing).
  // Lets a caller decide whether a relaunched search is "the same query" — so
  // find-next continues from the last result instead of restarting the scan —
  // using the matcher's full normalization rather than a partial ASCII compare.
  static bool queriesEquivalent(std::string_view a, std::string_view b);

  // Compile a query once for a book-wide search: normalize it and build the KMP
  // failure table over the result, so every page scan reuses one consistent
  // pattern + table. Returns false for an empty/oversized query or one that
  // normalizes to nothing (e.g. only spaces or hyphens).
  bool compile(std::string_view query);

  // Feed one byte into the matcher. Decodes UTF-8 and maps Latin diacritics.
  // Hyphens are ignored (fuzzy), as is the word-separator space a line-break
  // hyphenation leaves between a split word's halves; every other space is a
  // significant character that must be matched, so a query cannot cross a word
  // boundary it does not itself contain.
  //
  // Matches are whole-word: a hit must be delimited by non-word characters
  // (spaces, punctuation, the record's edges) on both sides, so "cat" no longer
  // matches inside "category". Because the trailing boundary can only be seen on
  // the following character, the return value is a small protocol rather than a
  // bare width:
  //   * 0  - nothing to report (also covers a rejected tentative match).
  //   * >0 - a leading-boundary-valid match just *completed* on this byte; the
  //          value is its raw byte width. The match is tentative until its
  //          trailing boundary is confirmed: the caller must record the span via
  //          setPendingMatchSpan() and keep feeding. A word character next door
  //          rejects it; a boundary (or the record's end, via the caller)
  //          confirms it.
  //   * <0 - the byte just fed is a word boundary that confirms the pending
  //          tentative match. The caller should report the span it recorded.
  int feed(uint8_t c);

  void reset() {
    matched = 0;
    utf8State = 0;
    utf8Codepoint = 0;
    utf8BytesConsumed = 0;
    pendingSeparatorBytes = 0;
    widthBufferHead = 0;
    currentCodepointId = 0;
    prevWasHyphen = false;
    lastEmittedWasSpace = false;
    prevWasWordChar = false;
    pendingActive = false;
  }

  // A partial match exists when KMP is mid-pattern or a completed match is still
  // awaiting its trailing-boundary confirmation. Both states want the wrapped
  // search to scan one continuation page so the match can finish.
  bool hasPartialMatch() const { return matched > 0 || pendingActive; }

  // True while the last significant byte fed was a hyphen, i.e. a line-break
  // hyphenation may still rejoin the current word with the next page's text. The
  // scan uses this to decide whether a record's end is a real word boundary.
  bool isHyphenPending() const { return prevWasHyphen; }

  // A completed-but-unconfirmed whole-word match is held until its trailing
  // boundary is seen. The caller owns the span coordinates (page + byte offsets)
  // since the matcher knows nothing about page layout; it just carries them so
  // they survive the matcher copies the chunked scan makes.
  bool hasPendingMatch() const { return pendingActive; }
  void setPendingMatchSpan(int page, int startByte, int endByte) {
    pendingActive = true;
    pendingPage_ = page;
    pendingStartByte_ = startByte;
    pendingEndByte_ = endByte;
  }
  int pendingPage() const { return pendingPage_; }
  int pendingStartByte() const { return pendingStartByte_; }
  int pendingEndByte() const { return pendingEndByte_; }

 private:
  std::array<uint8_t, MAX_QUERY_BYTES> pattern{};
  std::array<uint8_t, MAX_QUERY_BYTES> prefix{};
  size_t length = 0;
  size_t matched = 0;
  // Per-codepoint source byte widths. uint16_t (not uint8_t) so a matched span
  // whose ignored separators total more than 255 bytes cannot wrap and corrupt
  // the reported match width used for highlight offsets.
  std::array<uint16_t, MAX_QUERY_BYTES> matchByteWidths{};
  std::array<uint32_t, MAX_QUERY_BYTES> matchCodepointIds{};
  // Per-significant-byte flag: was the byte emitted just before this one a word
  // character? Read back at the match's start position on completion to decide
  // the leading word boundary without re-scanning. Same ring layout as the width
  // buffers above (indexed by widthBufferHead, wrapped at MAX_QUERY_BYTES).
  std::array<bool, MAX_QUERY_BYTES> precededByWordChar{};

  // Whether the compiled pattern begins / ends on a word character. A boundary
  // is only required on an edge that is itself a word char (regex \b semantics),
  // so a query like "(cat)" is not forced to sit between non-word characters.
  bool patternStartsWithWordChar = false;
  bool patternEndsWithWordChar = false;

  uint32_t utf8State = 0;
  uint32_t utf8Codepoint = 0;
  uint8_t utf8BytesConsumed = 0;
  uint16_t pendingSeparatorBytes = 0;
  uint8_t widthBufferHead = 0;
  uint32_t currentCodepointId = 0;
  // True when the previous codepoint was a dropped hyphen, so the next space is
  // treated as a line-break join and dropped. True when the last emitted byte
  // was a space, so runs of spaces collapse to one. Both span the byte stream
  // fed so far and are cleared by reset().
  bool prevWasHyphen = false;
  bool lastEmittedWasSpace = false;
  // Word-class of the most recent significant byte, used to fill
  // precededByWordChar for the next one. Reset to false (a non-word boundary) at
  // reset() so a fresh page or stream starts at a word boundary.
  bool prevWasWordChar = false;

  // A completed match whose trailing boundary has not yet been confirmed. The
  // span coordinates are caller-owned (see setPendingMatchSpan); the matcher
  // only tracks that one is outstanding so the next significant byte can confirm
  // or reject it. Survives the matcher copies the chunked scan makes.
  bool pendingActive = false;
  int pendingPage_ = -1;
  int pendingStartByte_ = 0;
  int pendingEndByte_ = 0;

  static size_t normalizeSearchQuery(std::string_view query, std::array<uint8_t, MAX_QUERY_BYTES>& out);
};
