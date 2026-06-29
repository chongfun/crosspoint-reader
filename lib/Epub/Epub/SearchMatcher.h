#pragma once

#include <array>
#include <cstdint>
#include <string_view>

class SearchMatcher {
 public:
  static constexpr size_t MAX_QUERY_BYTES = 64;

  std::array<uint8_t, MAX_QUERY_BYTES> pattern{};
  std::array<uint8_t, MAX_QUERY_BYTES> prefix{};
  size_t length = 0;
  size_t matched = 0;

  // Single source of truth for whether a query is usable for search: non-empty,
  // not all-whitespace, and within the byte limit. The UI validates with this
  // before launching a search.
  static bool isValidSearchQuery(std::string_view query);

  // Compile a query once for a book-wide search: normalize it and build the KMP
  // failure table over the result, so every page scan reuses one consistent
  // pattern + table. Returns false for an empty/oversized query or one that
  // normalizes to nothing (e.g. only spaces or hyphens).
  bool compile(std::string_view query);

  // Feed one character into the matcher. Returns true if a match is completed.
  // Separator characters (spaces and hyphens) are ignored and skipped.
  bool feed(uint8_t c);

  void reset() { matched = 0; }

 private:
  static size_t normalizeSearchQuery(std::string_view query, std::array<uint8_t, MAX_QUERY_BYTES>& out);
};
