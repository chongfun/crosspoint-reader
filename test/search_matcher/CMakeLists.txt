add_executable(SearchMatcherTest
  SearchMatcherTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/SearchMatcher.cpp
)

target_include_directories(SearchMatcherTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
)

target_link_libraries(SearchMatcherTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SearchMatcherTest)
