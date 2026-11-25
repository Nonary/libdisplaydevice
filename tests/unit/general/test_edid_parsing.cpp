// local includes
#include "display_device/types.h"
#include "fixtures/fixtures.h"

namespace {
  // Specialized TEST macro(s) for this test file
#define TEST_S(...) DD_MAKE_TEST(TEST, EdidParsing, __VA_ARGS__)
}  // namespace

TEST_S(NoData) {
  EXPECT_EQ(display_device::EdidData::parse({}), std::nullopt);
}

TEST_S(TooLittleData) {
  EXPECT_EQ(display_device::EdidData::parse({std::byte {0x11}}), std::nullopt);
}

TEST_S(BadFixedHeader) {
  auto EDID_DATA {ut_consts::DEFAULT_EDID};
  EDID_DATA[1] = std::byte {0xAA};
  EXPECT_EQ(display_device::EdidData::parse(EDID_DATA), std::nullopt);
}

TEST_S(BadChecksum) {
  auto EDID_DATA {ut_consts::DEFAULT_EDID};
  EDID_DATA[16] = std::byte {0x00};
  EXPECT_EQ(display_device::EdidData::parse(EDID_DATA), std::nullopt);
}

TEST_S(InvalidManufacturerId, BelowLimit) {
  auto EDID_DATA {ut_consts::DEFAULT_EDID};
  // The sum of 8th and 9th bytes should remain 109
  EDID_DATA[8] = std::byte {0x00};
  EDID_DATA[9] = std::byte {0x6D};
  EXPECT_EQ(display_device::EdidData::parse(EDID_DATA), std::nullopt);
}

TEST_S(InvalidManufacturerId, AboveLimit) {
  auto EDID_DATA {ut_consts::DEFAULT_EDID};
  // The sum of 8th and 9th bytes should remain 109
  EDID_DATA[8] = std::byte {0x6D};
  EDID_DATA[9] = std::byte {0x00};
  EXPECT_EQ(display_device::EdidData::parse(EDID_DATA), std::nullopt);
}

TEST_S(ValidOutput) {
  EXPECT_EQ(display_device::EdidData::parse(ut_consts::DEFAULT_EDID), ut_consts::DEFAULT_EDID_DATA);
}

TEST_S(QuietValidationPreservesParseResults) {
  using display_device::EdidData;
  using Level = display_device::Logger::LogLevel;

  auto malformed_edid {ut_consts::DEFAULT_EDID};
  // Keep the checksum valid but set the first manufacturer letter to zero.
  malformed_edid[8] = std::byte {0x00};
  malformed_edid[9] = std::byte {0x6D};

  auto &logger {display_device::Logger::get()};
  logger.setLogLevel(Level::verbose);
  std::vector<Level> diagnostics;
  logger.setCustomCallback([&diagnostics](Level level, const std::string &) {
    diagnostics.push_back(level);
  });

  EXPECT_EQ(EdidData::parse(ut_consts::DEFAULT_EDID, false), ut_consts::DEFAULT_EDID_DATA);
  EXPECT_EQ(EdidData::parse(malformed_edid, false), std::nullopt);
  EXPECT_TRUE(diagnostics.empty());
  EXPECT_EQ(EdidData::parse(malformed_edid), std::nullopt);
  EXPECT_EQ(diagnostics, std::vector {Level::warning});
}
