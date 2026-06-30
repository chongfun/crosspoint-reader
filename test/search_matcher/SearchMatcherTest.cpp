#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "SearchMatcher.h"

namespace {

// Result of a scan, mirroring the fields Section::scanForward reports.
struct ScanResult {
  bool matched = false;
  int page = -1;
  int startByte = -1;
  int endByte = -1;
};

// Drive the matcher over a book modelled as a list of per-page search-text
// records, reproducing Section::scanForward's feeding contract so these tests
// exercise the exact protocol the real scan relies on:
//   * an explicit word-boundary space is fed before each page's content,
//   * a positive feed() return is a tentative whole-word match whose span the
//     caller records and whose trailing boundary is confirmed later,
//   * a negative return confirms the pending match,
//   * the end of a record (no trailing separator) is a word boundary unless a
//     line-break hyphen is still pending, and
//   * an empty record is a discontinuity that also confirms a pending match.
// Keep this in lockstep with lib/Epub/Epub/SectionSearch.cpp; if that loop
// changes, this driver should change with it.
ScanResult scan(SearchMatcher& matcher, const std::vector<std::string>& pages) {
  for (size_t i = 0; i < pages.size(); ++i) {
    const std::string& record = pages[i];

    if (record.empty()) {
      if (matcher.hasPendingMatch()) {
        return {true, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
      }
      matcher.reset();
      continue;
    }

    if (matcher.feed(' ') < 0) {
      return {true, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
    }

    uint32_t pageBytePos = 0;
    for (const unsigned char ch : record) {
      const int signal = matcher.feed(ch);
      if (signal > 0) {
        const int endByte = static_cast<int>(pageBytePos);
        const int startByte = (pageBytePos + 1 >= static_cast<uint32_t>(signal))
                                  ? static_cast<int>(pageBytePos + 1 - static_cast<uint32_t>(signal))
                                  : 0;
        matcher.setPendingMatchSpan(static_cast<int>(i), startByte, endByte);
      } else if (signal < 0) {
        return {true, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
      }
      ++pageBytePos;
    }

    if (matcher.hasPendingMatch() && !matcher.isHyphenPending()) {
      return {true, matcher.pendingPage(), matcher.pendingStartByte(), matcher.pendingEndByte()};
    }
  }

  return {};
}

// Convenience: compile a query and scan a single-page book.
ScanResult scanQuery(const char* query, const std::vector<std::string>& pages) {
  SearchMatcher matcher;
  EXPECT_TRUE(matcher.compile(query)) << "query failed to compile: " << query;
  return scan(matcher, pages);
}

}  // namespace

// --- Whole-word boundaries: a substring inside a longer word must not match. ---

TEST(SearchMatcherWholeWord, RejectsSubstringInsideWord) {
  EXPECT_FALSE(scanQuery("cat", {"the category"}).matched);     // trailing word char
  EXPECT_FALSE(scanQuery("tegory", {"the category"}).matched);  // leading word char
  EXPECT_FALSE(scanQuery("ego", {"category"}).matched);         // both ends mid-word
  EXPECT_FALSE(scanQuery("cat", {"scat"}).matched);             // leading word char
  EXPECT_FALSE(scanQuery("x", {"the x2 fast"}).matched);        // single char inside word
}

TEST(SearchMatcherWholeWord, MatchesWholeWordsWithSpan) {
  const ScanResult cat = scanQuery("cat", {"the cat sat"});
  EXPECT_TRUE(cat.matched);
  EXPECT_EQ(cat.startByte, 4);
  EXPECT_EQ(cat.endByte, 6);

  const ScanResult category = scanQuery("category", {"the category here"});
  EXPECT_TRUE(category.matched);
  EXPECT_EQ(category.startByte, 4);
  EXPECT_EQ(category.endByte, 11);

  EXPECT_TRUE(scanQuery("the", {"the cat"}).matched);      // first word
  EXPECT_TRUE(scanQuery("sat", {"the cat sat"}).matched);  // last word, confirmed by record end
  EXPECT_TRUE(scanQuery("hi", {"hi"}).matched);            // whole record is the word
  EXPECT_TRUE(scanQuery("x2", {"the x2 fast"}).matched);   // digits are word chars
}

// --- Punctuation, parentheses, and record edges are boundaries. ---

TEST(SearchMatcherWholeWord, PunctuationIsABoundary) {
  EXPECT_TRUE(scanQuery("cat", {"a cat, here"}).matched);
  EXPECT_TRUE(scanQuery("cat", {"(cat)"}).matched);
  EXPECT_TRUE(scanQuery("cat", {"the cat."}).matched);
}

// A query that itself ends in a non-word char needs no trailing boundary
// (mirrors a regex \b, which only applies between word and non-word chars).
TEST(SearchMatcherWholeWord, NonWordQueryEdgeNeedsNoBoundary) {
  EXPECT_TRUE(scanQuery("etc.", {"and etc. more"}).matched);
  EXPECT_TRUE(scanQuery("etc.", {"foo etc.x more"}).matched);
}

// --- Phrases respect spaces and still require outer boundaries. ---

TEST(SearchMatcherPhrase, MatchesMultiWordQuery) {
  const ScanResult phrase = scanQuery("the cat", {"see the cat run"});
  EXPECT_TRUE(phrase.matched);
  EXPECT_EQ(phrase.startByte, 4);
  EXPECT_EQ(phrase.endByte, 10);
}

TEST(SearchMatcherPhrase, DoesNotCrossWordBoundaryWithoutSpace) {
  EXPECT_FALSE(scanQuery("heran", {"the rang"}).matched);
}

// --- Folding: case and Latin diacritics still resolve to whole-word matches. ---

TEST(SearchMatcherFolding, CaseAndDiacritics) {
  EXPECT_TRUE(scanQuery("CAT", {"the Cat sat"}).matched);
  EXPECT_TRUE(scanQuery("cafe", {"the caf\xC3\xA9 here"}).matched);  // café
}

// ß expands to "ss"; a prefix of that expanded word must not match, but the
// whole word must. This guards the in-fold trailing-boundary check.
TEST(SearchMatcherFolding, MultiByteExpansionRespectsBoundary) {
  EXPECT_FALSE(scanQuery("ma", {"the ma\xC3\x9F x"}).matched);  // "maß" -> "mass"
  EXPECT_TRUE(scanQuery("mass", {"a ma\xC3\x9F end"}).matched);
}

// --- Hyphenation stays fuzzy (the carve-out from whole-word matching). ---

TEST(SearchMatcherHyphenation, HardHyphenJoinsButPrefixStillNotWhole) {
  EXPECT_TRUE(scanQuery("motherinlaw", {"my mother-in-law cooks"}).matched);
  EXPECT_FALSE(scanQuery("mother", {"my mother-in-law cooks"}).matched);
}

// Layout line-break hyphen: "inter-" at the foot of one page, "national" at the
// top of the next. The match completes (and is reported) on the second page.
TEST(SearchMatcherHyphenation, LineBreakHyphenAcrossPage) {
  const ScanResult result = scanQuery("international", {"go inter-", "national now"});
  EXPECT_TRUE(result.matched);
  EXPECT_EQ(result.page, 1);
}

// --- Carried state across contiguous pages. ---

TEST(SearchMatcherCrossPage, PhraseStraddlesPageBoundary) {
  const ScanResult result = scanQuery("cat run", {"the cat", "run fast"});
  EXPECT_TRUE(result.matched);
  EXPECT_EQ(result.page, 1);  // reported where it completes
}

// Unmapped codepoints (e.g. CJK) are dropped on both sides and stay transparent
// for boundary purposes, like a hyphen.
TEST(SearchMatcherCrossPage, UnmappedCodepointIsTransparent) {
  // "a你b" -> "ab"; a search for "ab" matches it as a whole token.
  EXPECT_TRUE(scanQuery("ab", {"x a\xE4\xBD\xA0\x62 y"}).matched);
}

// --- Matching internals that the whole-word change must not regress. ---

// A self-overlapping query must not match inside a longer repetition, but must
// match the whole repeated token.
TEST(SearchMatcherInternals, SelfOverlapRespectsBoundary) {
  EXPECT_FALSE(scanQuery("aba", {"the ababa word"}).matched);
  EXPECT_TRUE(scanQuery("ababa", {"the ababa word"}).matched);
}

// The first whole-word occurrence wins, even when an earlier substring is
// rejected first.
TEST(SearchMatcherInternals, ReportsFirstWholeWordOccurrence) {
  const ScanResult firstOfTwo = scanQuery("cat", {"cat cat"});
  EXPECT_TRUE(firstOfTwo.matched);
  EXPECT_EQ(firstOfTwo.startByte, 0);
  EXPECT_EQ(firstOfTwo.endByte, 2);

  // "cat" appears as a prefix of "category" (rejected) before the standalone word.
  const ScanResult afterReject = scanQuery("cat", {"category cat"});
  EXPECT_TRUE(afterReject.matched);
  EXPECT_EQ(afterReject.startByte, 9);
  EXPECT_EQ(afterReject.endByte, 11);
}

// --- Query validation / equivalence helpers (unchanged behavior, guarded). ---

TEST(SearchMatcherQuery, ValidatesUsableQueries) {
  EXPECT_TRUE(SearchMatcher::isValidSearchQuery("cat"));
  EXPECT_FALSE(SearchMatcher::isValidSearchQuery(""));
  EXPECT_FALSE(SearchMatcher::isValidSearchQuery("   "));  // all whitespace
  EXPECT_FALSE(SearchMatcher::isValidSearchQuery("---"));  // normalizes to nothing
}

TEST(SearchMatcherQuery, EquivalenceIgnoresCaseAndFuzz) {
  EXPECT_TRUE(SearchMatcher::queriesEquivalent("Cat", "cat"));
  EXPECT_TRUE(SearchMatcher::queriesEquivalent("mother-in-law", "motherinlaw"));
  EXPECT_FALSE(SearchMatcher::queriesEquivalent("cat", "dog"));
}
