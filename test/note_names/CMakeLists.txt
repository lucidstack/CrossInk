add_executable(NoteNamesTest
  NoteNamesTest.cpp
)

target_include_directories(NoteNamesTest PRIVATE
  ${REPO_ROOT}/src/editor
)

target_link_libraries(NoteNamesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(NoteNamesTest)
