// Exercise the production setters against a stateful Windows API boundary.
// The saved database intentionally starts different from the live session:
// any accidental SDC_SAVE_TO_DATABASE, including a rollback, corrupts it.
#include "display_device/windows/win_display_device.h"
#include "utils/comparison.h"
#include "utils/mock_win_api_layer.h"

#include <gmock/gmock.h>

namespace {
  using ::testing::_;
  using ::testing::Invoke;
  using ::testing::NiceMock;
  using ::testing::Return;

  class TemporaryDisplayPersistence: public ::testing::Test {
  protected:
    void SetUp() override {
      saved.m_paths.clear();
      saved.m_modes.clear();
      ON_CALL(*api, queryDisplayConfig(_)).WillByDefault(Invoke([this](auto) {
        return std::make_optional(live);
      }));
      ON_CALL(*api, getDeviceId(_)).WillByDefault(Invoke([](const auto &path) {
        return "DeviceId" + std::to_string(path.sourceInfo.id);
      }));
      ON_CALL(*api, getMonitorDevicePath(_)).WillByDefault(Invoke([](const auto &path) {
        return "MonitorPath" + std::to_string(path.sourceInfo.id);
      }));
      ON_CALL(*api, getDisplayName(_)).WillByDefault(Invoke([](const auto &path) {
        return "DisplayName" + std::to_string(path.sourceInfo.id);
      }));
      ON_CALL(*api, getSupportedDisplayModes(_))
        .WillByDefault(Return(std::vector<display_device::DisplayMode> {requested.at("DeviceId1")}));
      ON_CALL(*api, setDisplayConfig(_, _, _)).WillByDefault(Invoke([this](auto paths, auto modes, UINT32 flags) {
        apply(std::move(paths), std::move(modes), flags);
        return ERROR_SUCCESS;
      }));
    }

    void apply(std::vector<DISPLAYCONFIG_PATH_INFO> paths, std::vector<DISPLAYCONFIG_MODE_INFO> modes, UINT32 flags) {
      writes.push_back(flags);
      live.m_paths = std::move(paths);
      live.m_modes = std::move(modes);
      if (flags & SDC_SAVE_TO_DATABASE) {
        saved = live;
      }
    }

    void expectDatabaseUntouched() {
      EXPECT_TRUE(saved.m_paths.empty());
      EXPECT_TRUE(saved.m_modes.empty());
      for (const auto flags : writes) {
        EXPECT_EQ(flags & SDC_SAVE_TO_DATABASE, 0u);
        EXPECT_NE(flags & SDC_APPLY, 0u);
      }
    }

    std::shared_ptr<NiceMock<display_device::MockWinApiLayer>> api {
      std::make_shared<NiceMock<display_device::MockWinApiLayer>>()};
    display_device::WinDisplayDevice device {api};
    display_device::PathAndModeData live {*ut_consts::PAM_3_ACTIVE};
    display_device::PathAndModeData saved;
    std::vector<UINT32> writes;
    const display_device::DeviceDisplayModeMap requested {{"DeviceId1", {{1280, 720}, {60, 1}}}};
  };
}  // namespace

TEST_F(TemporaryDisplayPersistence, ModeSuccessDoesNotSaveSessionTopology) {
  EXPECT_TRUE(device.setDisplayModesTemporary(requested));
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_NE(writes.front() & SDC_ALLOW_CHANGES, 0u);
  EXPECT_EQ(device.getCurrentDisplayModes({"DeviceId1"}), requested);
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, StrictRetryAfterSubstitutedModeIsTemporary) {
  EXPECT_CALL(*api, setDisplayConfig(_, _, _)).Times(2)
    .WillOnce(Invoke([this](auto paths, auto modes, UINT32 flags) {
      paths.front().targetInfo.refreshRate = {75, 1};
      apply(std::move(paths), std::move(modes), flags);
      return ERROR_SUCCESS;
    }))
    .WillOnce(Invoke([this](auto paths, auto modes, UINT32 flags) {
      apply(std::move(paths), std::move(modes), flags);
      return ERROR_SUCCESS;
    }));

  EXPECT_TRUE(device.setDisplayModesTemporary(requested));
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_NE(writes.front() & SDC_ALLOW_CHANGES, 0u);
  EXPECT_EQ(writes.back() & SDC_ALLOW_CHANGES, 0u);
  EXPECT_EQ(device.getCurrentDisplayModes({"DeviceId1"}), requested);
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, FailedStrictRetryRestoresWholeOriginalLiveConfigurationTemporarily) {
  const auto original = live;
  EXPECT_CALL(*api, setDisplayConfig(_, _, _)).Times(3)
    .WillOnce(Invoke([this](auto paths, auto modes, UINT32 flags) {
      paths.front().targetInfo.refreshRate = {75, 1};
      // Windows can also reposition another monitor during a relaxed apply.
      modes.back().sourceMode.position.x += 100;
      apply(std::move(paths), std::move(modes), flags);
      return ERROR_SUCCESS;
    }))
    .WillOnce(Invoke([this](auto, auto, UINT32 flags) {
      writes.push_back(flags);
      return ERROR_ACCESS_DENIED;
    }))
    .WillOnce(Invoke([this](auto paths, auto modes, UINT32 flags) {
      apply(std::move(paths), std::move(modes), flags);
      return ERROR_SUCCESS;
    }));

  EXPECT_FALSE(device.setDisplayModesTemporary(requested));
  EXPECT_EQ(live.m_paths, original.m_paths);
  EXPECT_EQ(live.m_modes, original.m_modes);
  ASSERT_EQ(writes.size(), 3u);
  EXPECT_EQ(writes.back(), (SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE));
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, FailedReadbackStillRollsBackWithoutSaving) {
  const auto original = live;
  ON_CALL(*api, queryDisplayConfig(_)).WillByDefault(Invoke([this](auto) -> std::optional<display_device::PathAndModeData> {
    if (writes.size() == 1) {
      return std::nullopt;
    }
    return live;
  }));

  EXPECT_FALSE(device.setDisplayModesTemporary(requested));
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(live.m_paths, original.m_paths);
  EXPECT_EQ(live.m_modes, original.m_modes);
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, FailedRollbackNeverReportsSuccessOrSavesDatabase) {
  EXPECT_CALL(*api, setDisplayConfig(_, _, _)).Times(3)
    .WillOnce(Invoke([this](auto paths, auto modes, UINT32 flags) {
      paths.front().targetInfo.refreshRate = {75, 1};
      apply(std::move(paths), std::move(modes), flags);
      return ERROR_SUCCESS;
    }))
    .WillRepeatedly(Invoke([this](auto, auto, UINT32 flags) {
      writes.push_back(flags);
      return ERROR_ACCESS_DENIED;
    }));

  EXPECT_FALSE(device.setDisplayModesTemporary(requested));
  ASSERT_EQ(writes.size(), 3u);
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, MissingRollbackSnapshotPreventsMutation) {
  unsigned int queries = 0;
  ON_CALL(*api, queryDisplayConfig(_)).WillByDefault(Invoke([this, &queries](auto) -> std::optional<display_device::PathAndModeData> {
    // Duplicates, current modes, resolution selection, then rollback capture.
    return ++queries == 4 ? std::nullopt : std::make_optional(live);
  }));
  EXPECT_CALL(*api, setDisplayConfig(_, _, _)).Times(0);

  EXPECT_FALSE(device.setDisplayModesTemporary(requested));
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, MatchingModesDoNotWriteConfiguration) {
  const auto current = device.getCurrentDisplayModes({"DeviceId1"});
  EXPECT_CALL(*api, setDisplayConfig(_, _, _)).Times(0);

  EXPECT_TRUE(device.setDisplayModesTemporary(current));
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, InvalidRefreshRateFailsBeforeQueryOrMutation) {
  EXPECT_CALL(*api, queryDisplayConfig(_)).Times(0);
  EXPECT_CALL(*api, setDisplayConfig(_, _, _)).Times(0);

  EXPECT_FALSE(device.setDisplayModesTemporary({{"DeviceId1", {{1280, 720}, {60, 0}}}}));
  EXPECT_FALSE(device.setDisplayModesTemporary({{"DeviceId1", {{1280, 720}, {0, 1}}}}));
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, SessionPrimaryAndOriginChangesDoNotSave) {
  EXPECT_TRUE(device.setAsPrimaryTemporary("DeviceId2"));
  EXPECT_TRUE(device.setDisplayOriginTemporary("DeviceId1", {-1280, 0}));
  ASSERT_EQ(writes.size(), 2u);
  expectDatabaseUntouched();
}

TEST_F(TemporaryDisplayPersistence, ExplicitPersistentModePrimaryAndOriginStillSave) {
  EXPECT_TRUE(device.setDisplayModes(requested));
  EXPECT_TRUE(device.setAsPrimary("DeviceId2"));
  EXPECT_TRUE(device.setDisplayOrigin("DeviceId1", {-1280, 0}));
  ASSERT_EQ(writes.size(), 3u);
  for (const auto flags : writes) {
    EXPECT_NE(flags & SDC_SAVE_TO_DATABASE, 0u);
  }
  EXPECT_EQ(saved.m_paths, live.m_paths);
  EXPECT_EQ(saved.m_modes, live.m_modes);
}
