#pragma once

#include <cstddef>
#include <cstdint>

// On-disk section cache format constants, shared between the cache
// writer/reader (Section.cpp) and the forward search scanner
// (SectionSearch.cpp). Keeping the header size and page-LUT stride in one place
// means the two translation units cannot drift apart on the binary layout.
namespace epub {

constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
// v42: page LUT entries include offsets to compact text records used by search,
//      and corrected SD-card font advance measurement in CJK-heavy layouts.
constexpr uint8_t SECTION_FILE_VERSION = 42;

constexpr uint32_t HEADER_SIZE = sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) +
                                 sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                 sizeof(bool) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);

// On-disk page LUT stride: only pageOffset and searchTextOffset are stored
// inline; paragraphIndex and listItemIndex are written to separate LUTs.
constexpr size_t PAGE_LUT_ENTRY_SIZE = sizeof(uint32_t) * 2;

}  // namespace epub
