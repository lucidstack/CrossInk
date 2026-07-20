add_executable(TextEditorTest
  TextEditorTest.cpp
  ${REPO_ROOT}/src/editor/text_editor.cpp
)

target_include_directories(TextEditorTest PRIVATE
  ${REPO_ROOT}/src/editor
)

target_link_libraries(TextEditorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextEditorTest)
