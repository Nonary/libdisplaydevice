/**
 * @file src/windows/include/display_device/windows/display_mode_resolution.h
 * @brief Helpers for combining and resolving Windows display modes.
 */
#pragma once

// system includes
#include <optional>
#include <vector>

// local includes
#include "types.h"

namespace display_device::win_utils {
  /**
   * @brief Merge two supported-mode lists, keeping primary entries first.
   *
   * Used when DXGI omitted a mode that a targeted GDI probe later confirmed.
   * Modes are considered the same when width, height, and the refresh-rate
   * fraction match exactly. Secondary modes that are not already present are
   * appended.
   *
   * @param primary Preferred mode list (DXGI).
   * @param secondary Additional modes (for example a GDI-probed custom mode).
   * @return Combined list with duplicates from @p secondary removed.
   */
  [[nodiscard]] std::vector<DisplayMode> mergeDisplayModes(const std::vector<DisplayMode> &primary, const std::vector<DisplayMode> &secondary);

  /**
   * @brief Check whether any supported mode has the requested width and height.
   * @param modes Mode list to search (typically DXGI).
   * @param resolution Resolution to look for.
   * @return True if at least one mode matches the resolution, regardless of refresh.
   */
  [[nodiscard]] bool supportedModesContainResolution(const std::vector<DisplayMode> &modes, const Resolution &resolution);

  /**
   * @brief Resolve a requested mode against the modes Windows reports as supported.
   *
   * Preference order:
   *  1. Exact resolution and refresh-rate match.
   *  2. Closest same-aspect-ratio mode (also covers the same resolution at a different refresh).
   *  3. The display's preferred resolution, but only when it has the same aspect ratio.
   *  4. The originally requested mode.
   *
   * A different-aspect-ratio "closest pixel area" fallback is intentionally not used.
   * That path would turn a custom 3840x1680 virtual-display mode into 5120x1440 when
   * DXGI omits the custom entry but still advertises the driver's stock ultrawide mode.
   *
   * @param requested_mode Mode the caller asked for.
   * @param supported_modes Modes reported as supported for the target.
   * @param preferred_resolution Optional OS-preferred resolution for the target.
   * @return Mode that should be applied.
   */
  [[nodiscard]] DisplayMode resolveRequestedDisplayMode(const DisplayMode &requested_mode, const std::vector<DisplayMode> &supported_modes, const std::optional<Resolution> &preferred_resolution);
}  // namespace display_device::win_utils
