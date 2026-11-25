#include <algorithm>

#include "display_device/windows/win_display_device.h"
#include "fixtures/fixtures.h"
#include "utils/mock_win_api_layer.h"

TEST(WinDisplayDeviceEdid, MalformedEdidWarnsOnlyWhenTargetIsActive) {
  using Level = display_device::Logger::LogLevel;
  using ::testing::_;
  using ::testing::ElementsAre;
  using ::testing::NiceMock;
  using ::testing::Return;

  auto paths {ut_consts::PAM_3_ACTIVE};
  auto &malformed_path {paths->m_paths[1]};
  auto &valid_path {paths->m_paths[2]};
  malformed_path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
  valid_path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;

  auto malformed_edid {ut_consts::DEFAULT_EDID};
  // Keep the checksum valid but set the first manufacturer letter to zero.
  malformed_edid[8] = std::byte {0x00};
  malformed_edid[9] = std::byte {0x6D};

  const auto api {std::make_shared<NiceMock<display_device::MockWinApiLayer>>()};
  ON_CALL(*api, queryDisplayConfig(display_device::QueryType::All)).WillByDefault([&paths] { return paths; });
  ON_CALL(*api, getMonitorDevicePath(_)).WillByDefault([](const DISPLAYCONFIG_PATH_INFO &path) {
    return "Path" + std::to_string(path.sourceInfo.id);
  });
  ON_CALL(*api, getDeviceId(_)).WillByDefault([](const DISPLAYCONFIG_PATH_INFO &path) {
    return "Device" + std::to_string(path.sourceInfo.id);
  });
  ON_CALL(*api, getDisplayName(_)).WillByDefault([](const DISPLAYCONFIG_PATH_INFO &path) {
    return "Display" + std::to_string(path.sourceInfo.id);
  });
  ON_CALL(*api, getDisplayScale(_, _)).WillByDefault(Return(display_device::Rational {1, 1}));
  ON_CALL(*api, getEdid(_)).WillByDefault([&](const DISPLAYCONFIG_PATH_INFO &path) {
    return path.sourceInfo.id == malformed_path.sourceInfo.id ? malformed_edid : ut_consts::DEFAULT_EDID;
  });

  auto &logger {display_device::Logger::get()};
  logger.setLogLevel(Level::verbose);
  std::vector<Level> diagnostics;
  logger.setCustomCallback([&](Level level, const std::string &message) {
    if (message.starts_with("EDID manufacturer id is out of range.")) {
      diagnostics.push_back(level);
    }
  });

  const auto malformed_device_id {"Device" + std::to_string(malformed_path.sourceInfo.id)};
  const auto valid_device_id {"Device" + std::to_string(valid_path.sourceInfo.id)};
  constexpr auto minimal {display_device::DeviceEnumerationDetail::Minimal};
  for (int poll = 0; poll < 3; ++poll) {
    const display_device::WinDisplayDevice device {api};
    const auto outputs {device.enumAvailableDevices(minimal)};

    const auto malformed {std::ranges::find(outputs, malformed_device_id, &display_device::EnumeratedDevice::m_device_id)};
    ASSERT_NE(malformed, outputs.end());
    EXPECT_FALSE(malformed->m_info);
    EXPECT_FALSE(malformed->m_edid);

    const auto valid {std::ranges::find(outputs, valid_device_id, &display_device::EnumeratedDevice::m_device_id)};
    ASSERT_NE(valid, outputs.end());
    EXPECT_FALSE(valid->m_info);
    EXPECT_EQ(valid->m_edid, ut_consts::DEFAULT_EDID_DATA);
  }
  EXPECT_TRUE(diagnostics.empty());

  malformed_path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
  const display_device::WinDisplayDevice device {api};
  const auto outputs {device.enumAvailableDevices(minimal)};
  const auto active {std::ranges::find(outputs, malformed_device_id, &display_device::EnumeratedDevice::m_device_id)};
  ASSERT_NE(active, outputs.end());
  EXPECT_TRUE(active->m_info);
  EXPECT_FALSE(active->m_edid);
  EXPECT_THAT(diagnostics, ElementsAre(Level::warning));
}
