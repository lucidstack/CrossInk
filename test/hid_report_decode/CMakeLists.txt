add_executable(HidReportDecodeTest
  HidReportDecodeTest.cpp
)

target_include_directories(HidReportDecodeTest PRIVATE
  ${REPO_ROOT}/freeink-sdk/libs/network/BleKeyboardHost/src
)

target_link_libraries(HidReportDecodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HidReportDecodeTest)
