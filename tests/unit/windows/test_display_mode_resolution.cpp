// local includes
#include "display_device/windows/display_mode_resolution.h"
#include "fixtures/fixtures.h"

namespace {
  using display_device::DisplayMode;
  using display_device::Resolution;
  using display_device::win_utils::mergeDisplayModes;
  using display_device::win_utils::resolveRequestedDisplayMode;
  using display_device::win_utils::supportedModesContainResolution;

  // Specialized TEST macro(s) for this test file
#define TEST_S(...) DD_MAKE_TEST(TEST, DisplayModeResolution, __VA_ARGS__)

  const DisplayMode kCustomVddMode {3840, 1680, {60, 1}};
  const DisplayMode kCustomVddMode1690 {3840, 1690, {60, 1}};
  const DisplayMode kStockQhd {2560, 1440, {60, 1}};
  const DisplayMode kStock4k {3840, 2160, {60, 1}};
  const DisplayMode kStockSuperUltrawide {5120, 1440, {60, 1}};
  const DisplayMode kStock1080p {1920, 1080, {60, 1}};
  const DisplayMode kStock1080p120 {1920, 1080, {120, 1}};

  const std::vector<DisplayMode> kDxgiStockModes {
    kStock1080p,
    kStockQhd,
    kStock4k,
    kStockSuperUltrawide
  };

  const std::vector<DisplayMode> kGdiModesWithCustom {
    kStock1080p,
    kStockQhd,
    kCustomVddMode,
    kStock4k,
    kStockSuperUltrawide
  };

  bool containsMode(const std::vector<DisplayMode> &modes, const DisplayMode &expected) {
    for (const auto &mode : modes) {
      if (mode == expected) {
        return true;
      }
    }
    return false;
  }
}  // namespace

TEST_S(MergeDisplayModes, GdiCustomModeIsKeptWhenDxgiOmitsIt) {
  const auto merged {mergeDisplayModes(kDxgiStockModes, {kCustomVddMode})};

  EXPECT_TRUE(containsMode(merged, kCustomVddMode));
  EXPECT_TRUE(containsMode(merged, kStockSuperUltrawide));
  EXPECT_TRUE(containsMode(merged, kStock4k));
  EXPECT_EQ(merged.size(), kDxgiStockModes.size() + 1);
}

TEST_S(SupportedModesContainResolution, DxgiStockModesOmitCustomVddMode) {
  EXPECT_FALSE(supportedModesContainResolution(kDxgiStockModes, kCustomVddMode.m_resolution));
  EXPECT_TRUE(supportedModesContainResolution(kDxgiStockModes, kStockSuperUltrawide.m_resolution));
}

TEST_S(ResolveRequestedDisplayMode, HybridProbeMergesOnlyTheRequestedCustomMode) {
  ASSERT_FALSE(supportedModesContainResolution(kDxgiStockModes, kCustomVddMode.m_resolution));
  const auto supported {mergeDisplayModes(kDxgiStockModes, {kCustomVddMode})};
  const auto resolved {resolveRequestedDisplayMode(kCustomVddMode, supported, Resolution {3840, 2160})};

  EXPECT_EQ(resolved, kCustomVddMode);
}

TEST_S(MergeDisplayModes, EmptyDxgiUsesGdi) {
  const auto merged {mergeDisplayModes({}, kGdiModesWithCustom)};
  EXPECT_EQ(merged, kGdiModesWithCustom);
}

TEST_S(MergeDisplayModes, EmptyGdiKeepsDxgi) {
  const auto merged {mergeDisplayModes(kDxgiStockModes, {})};
  EXPECT_EQ(merged, kDxgiStockModes);
}

TEST_S(MergeDisplayModes, DuplicateModesAreNotRepeated) {
  const auto merged {mergeDisplayModes(kDxgiStockModes, kDxgiStockModes)};
  EXPECT_EQ(merged, kDxgiStockModes);
}

TEST_S(ResolveRequestedDisplayMode, CustomModeFromGdiIsPreferredOverCloserPixelCount) {
  const auto supported {mergeDisplayModes(kDxgiStockModes, {kCustomVddMode})};
  const auto resolved {resolveRequestedDisplayMode(kCustomVddMode, supported, Resolution {3840, 2160})};

  EXPECT_EQ(resolved, kCustomVddMode);
}

TEST_S(ResolveRequestedDisplayMode, DoesNotChangeAspectRatioWhenCustomModeMissing) {
  // DXGI-only list: the old "closest pixel area" fallback would pick 5120x1440
  // because it is closer in total pixels than 3840x2160. Keep the requested mode.
  const auto resolved {resolveRequestedDisplayMode(kCustomVddMode, kDxgiStockModes, Resolution {3840, 2160})};

  EXPECT_EQ(resolved.m_resolution.m_width, 3840U);
  EXPECT_EQ(resolved.m_resolution.m_height, 1680U);
  EXPECT_NE(resolved, kStockSuperUltrawide);
}

TEST_S(ResolveRequestedDisplayMode, NearbyCustomHeightDoesNotSelectSuperUltrawide) {
  const auto resolved {resolveRequestedDisplayMode(kCustomVddMode1690, kDxgiStockModes, Resolution {3840, 2160})};

  EXPECT_EQ(resolved.m_resolution.m_width, 3840U);
  EXPECT_EQ(resolved.m_resolution.m_height, 1690U);
  EXPECT_NE(resolved, kStockSuperUltrawide);
}

TEST_S(ResolveRequestedDisplayMode, ExactMatchIsReturned) {
  const auto resolved {resolveRequestedDisplayMode(kStock4k, kDxgiStockModes, Resolution {3840, 2160})};
  EXPECT_EQ(resolved, kStock4k);
}

TEST_S(ResolveRequestedDisplayMode, SameAspectClosestIsUsedWhenExactMissing) {
  const DisplayMode requested {1600, 900, {60, 1}};  // 16:9, not in the stock list
  const auto resolved {resolveRequestedDisplayMode(requested, kDxgiStockModes, Resolution {3840, 2160})};

  EXPECT_EQ(resolved, kStock1080p);
}

TEST_S(ResolveRequestedDisplayMode, SameResolutionDifferentRefreshIsPreferred) {
  const std::vector<DisplayMode> supported {
    kStock1080p,
    kStock1080p120,
    kStockSuperUltrawide
  };
  const DisplayMode requested {1920, 1080, {144, 1}};
  const auto resolved {resolveRequestedDisplayMode(requested, supported, std::nullopt)};

  EXPECT_EQ(resolved, kStock1080p120);
}
