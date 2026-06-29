#include "SearchMatcher.h"

#include <algorithm>
#include <cctype>

#include "AsciiCase.h"

namespace {
constexpr bool isSearchSeparator(const uint8_t b) { return b == ' ' || b == '-'; }
}  // namespace

bool SearchMatcher::isValidSearchQuery(const std::string_view query) {
  if (query.empty() || query.size() > MAX_QUERY_BYTES) {
    return false;
  }
  // Require at least one byte that survives normalization. The matcher ignores
  // ASCII spaces and hyphens (see normalizeSearchQuery), so a query of only
  // whitespace and/or hyphens normalizes to nothing and can never match; the UI
  // gate must agree with the matcher on what is searchable.
  return std::any_of(query.begin(), query.end(),
                     [](const unsigned char value) { return std::isspace(value) == 0 && !isSearchSeparator(value); });
}

size_t SearchMatcher::normalizeSearchQuery(const std::string_view query, std::array<uint8_t, MAX_QUERY_BYTES>& out) {
  size_t len = 0;
  for (const char c : query) {
    const uint8_t b = static_cast<uint8_t>(c);
    if (isSearchSeparator(b)) {
      continue;
    }
    if (len >= out.size()) {
      break;  // defensive; callers reject query.size() > MAX_QUERY_BYTES
    }
    out[len++] = epub::asciiToLower(b);
  }
  return len;
}

bool SearchMatcher::compile(const std::string_view query) {
  // Leave the result in a defined (zeroed, length 0) state even on rejection, so
  // a caller that ignores the return value never matches against stale bytes.
  pattern.fill(0);
  prefix.fill(0);
  length = 0;
  matched = 0;

  // One validity definition (empty / oversized / no searchable byte) shared with
  // the UI gate; a query that passes always normalizes to a non-empty pattern.
  if (!isValidSearchQuery(query)) {
    return false;
  }

  // Normalize once (lowercase, spaces/hyphens dropped); the KMP table is built
  // over that same pattern, so pattern and prefix can never disagree.
  length = normalizeSearchQuery(query, pattern);

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

bool SearchMatcher::feed(uint8_t c) {
  if (isSearchSeparator(c)) {
    return false;  // spaces/hyphens are insignificant on both sides
  }
  const uint8_t value = epub::asciiToLower(c);
  while (matched > 0 && value != pattern[matched]) {
    matched = prefix[matched - 1];
  }
  if (value == pattern[matched]) {
    ++matched;
    if (matched == length) {
      return true;
    }
  }
  return false;
}
