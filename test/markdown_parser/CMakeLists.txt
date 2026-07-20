add_executable(MarkdownParserTest
  MarkdownParserTest.cpp
)

target_include_directories(MarkdownParserTest PRIVATE
  ${REPO_ROOT}/src/editor
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(MarkdownParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MarkdownParserTest)
