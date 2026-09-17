/**
 * @file src/windows/win_display_device_modes.cpp
 * @brief Definitions for the display mode related methods in WinDisplayDevice.
 */
// class header include
#include "display_device/windows/win_display_device.h"

// local includes
#include "display_device/logging.h"
#include "display_device/windows/display_mode_resolution.h"
#include "display_device/windows/win_api_utils.h"

namespace display_device {
  namespace {

    /**
     * @brief Strategy to be used when changing display modes.
     */
    enum class Strategy {
      Relaxed,
      Strict
    };

    enum class PersistPolicy {
      SaveToDatabase,
      Temporary
    };

    /**
     * @see set_display_modes for a description as this was split off to reduce cognitive complexity.
     */
    bool doSetModes(WinApiLayerInterface &w_api, const DeviceDisplayModeMap &modes, const Strategy strategy, const PersistPolicy persist) {
      auto display_data {w_api.queryDisplayConfig(QueryType::Active)};
      if (!display_data) {
        // Error already logged
        return false;
      }

      bool changes_applied {false};
      for (const auto &[device_id, mode] : modes) {
        const auto path {win_utils::getActivePath(w_api, device_id, display_data->m_paths)};
        if (!path) {
          DD_LOG(error) << "Failed to find device for " << device_id << "!";
          return false;
        }

        const auto source_mode {win_utils::getSourceMode(win_utils::getSourceIndex(*path, display_data->m_modes), display_data->m_modes)};
        if (!source_mode) {
          DD_LOG(error) << "Active device does not have a source mode: " << device_id << "!";
          return false;
        }

        bool new_changes {false};
        const bool resolution_changed {source_mode->width != mode.m_resolution.m_width || source_mode->height != mode.m_resolution.m_height};

        bool refresh_rate_changed;
        if (strategy == Strategy::Relaxed) {
          refresh_rate_changed = !win_utils::fuzzyCompareRefreshRates(Rational {path->targetInfo.refreshRate.Numerator, path->targetInfo.refreshRate.Denominator}, mode.m_refresh_rate);
        } else {
          refresh_rate_changed = path->targetInfo.refreshRate.Numerator != mode.m_refresh_rate.m_numerator ||
                                 path->targetInfo.refreshRate.Denominator != mode.m_refresh_rate.m_denominator;
        }

        if (resolution_changed) {
          source_mode->width = mode.m_resolution.m_width;
          source_mode->height = mode.m_resolution.m_height;
          new_changes = true;
        }

        if (refresh_rate_changed) {
          path->targetInfo.refreshRate = {mode.m_refresh_rate.m_numerator, mode.m_refresh_rate.m_denominator};
          new_changes = true;
        }

        if (new_changes) {
          win_utils::setTargetIndex(*path, std::nullopt);
          win_utils::setDesktopIndex(*path, std::nullopt);
        }

        changes_applied = changes_applied || new_changes;
      }

      if (!changes_applied) {
        DD_LOG(debug) << "No changes were made to display modes as they are equal.";
        return true;
      }

      UINT32 flags {SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE};
      if (persist == PersistPolicy::SaveToDatabase) {
        flags |= SDC_SAVE_TO_DATABASE;
      }
      if (strategy == Strategy::Relaxed) {
        flags |= SDC_ALLOW_CHANGES;
      }

      const LONG result {w_api.setDisplayConfig(display_data->m_paths, display_data->m_modes, flags)};
      if (result != ERROR_SUCCESS) {
        DD_LOG(error) << w_api.getErrorString(result) << " failed to set display mode!";
        return false;
      }

      return true;
    }

    struct ResolvedModes {
      DeviceDisplayModeMap resolved;
      bool requires_apply {false};
    };

    std::optional<ResolvedModes> resolveRequestedModes(WinApiLayerInterface &w_api, const DeviceDisplayModeMap &requested_modes, const std::set<std::string> &device_ids, const DeviceDisplayModeMap &current_modes) {
      const auto display_active {w_api.queryDisplayConfig(QueryType::Active)};
      if (!display_active) {
        return std::nullopt;
      }

      ResolvedModes result;

      for (const auto &device_id : device_ids) {
        const auto request_it {requested_modes.find(device_id)};
        if (request_it == std::end(requested_modes)) {
          DD_LOG(error) << "Requested mode for device " << device_id << " is missing!";
          return std::nullopt;
        }

        const auto &requested_mode {request_it->second};

        auto current_it {current_modes.find(device_id)};
        if (current_it != std::end(current_modes) && win_utils::fuzzyCompareModes(current_it->second, requested_mode)) {
          result.resolved[device_id] = requested_mode;
          continue;
        }

        const auto path {win_utils::getActivePath(w_api, device_id, display_active->m_paths)};
        if (!path) {
          DD_LOG(error) << "Failed to find device for " << device_id << " while resolving modes!";
          return std::nullopt;
        }

        auto supported {w_api.getSupportedDisplayModes(*path)};
        if (!win_utils::supportedModesContainResolution(supported, requested_mode.m_resolution)) {
          // DXGI can omit a dynamically advertised custom mode. Probe that one mode
          // via GDI instead of walking the entire GDI list (which is seconds-slow
          // on some virtual display drivers).
          if (w_api.probeGdiDisplayMode(*path, requested_mode)) {
            DD_LOG(debug) << "GDI accepted requested mode "
                          << requested_mode.m_resolution.m_width << "x" << requested_mode.m_resolution.m_height
                          << " that DXGI omitted for device " << device_id << ".";
            supported = win_utils::mergeDisplayModes(supported, {requested_mode});
          }
        }

        const auto preferred_resolution {w_api.getPreferredResolution(*path)};
        const DisplayMode final_mode {win_utils::resolveRequestedDisplayMode(requested_mode, supported, preferred_resolution)};

        if (!win_utils::fuzzyCompareModes(final_mode, requested_mode)) {
          DD_LOG(info) << "Resolved display mode for device " << device_id << " adjusted to: "
                       << final_mode.m_resolution.m_width << "x" << final_mode.m_resolution.m_height << " @ "
                       << final_mode.m_refresh_rate.m_numerator << "/" << final_mode.m_refresh_rate.m_denominator;
        } else if (supported.empty()) {
          DD_LOG(warning) << "No supported fallback modes for device " << device_id << ". Using requested mode.";
        }

        const bool matches_current = current_it != std::end(current_modes) && win_utils::fuzzyCompareModes(current_it->second, final_mode);
        if (!matches_current) {
          result.requires_apply = true;
        }

        result.resolved[device_id] = final_mode;
      }

      return result;
    }

    bool modesMatch(const DeviceDisplayModeMap &lhs, const DeviceDisplayModeMap &rhs) {
      for (const auto &[device_id, expected_mode] : rhs) {
        auto current_it {lhs.find(device_id)};
        if (current_it == std::end(lhs)) {
          return false;
        }
        if (!win_utils::fuzzyCompareModes(current_it->second, expected_mode)) {
          return false;
        }
      }

      return true;
    }
  }  // namespace

  DeviceDisplayModeMap WinDisplayDevice::getCurrentDisplayModes(const std::set<std::string> &device_ids) const {
    if (device_ids.empty()) {
      DD_LOG(error) << "Device id set is empty!";
      return {};
    }

    const auto display_data {m_w_api->queryDisplayConfig(QueryType::Active)};
    if (!display_data) {
      return {};
    }

    DeviceDisplayModeMap current_modes;
    for (const auto &device_id : device_ids) {
      if (device_id.empty()) {
        DD_LOG(error) << "Device id is empty!";
        return {};
      }

      const auto path {win_utils::getActivePath(*m_w_api, device_id, display_data->m_paths)};
      if (!path) {
        DD_LOG(error) << "Failed to find device for " << device_id << "!";
        return {};
      }

      const auto source_mode {win_utils::getSourceMode(win_utils::getSourceIndex(*path, display_data->m_modes), display_data->m_modes)};
      if (!source_mode) {
        DD_LOG(error) << "Active device does not have a source mode: " << device_id << "!";
        return {};
      }

      const auto target_refresh_rate {path->targetInfo.refreshRate};
      current_modes[device_id] = DisplayMode {
        {source_mode->width, source_mode->height},
        {target_refresh_rate.Numerator, target_refresh_rate.Denominator}
      };
    }

    return current_modes;
  }

  bool WinDisplayDevice::setDisplayModes(const DeviceDisplayModeMap &modes) {
    if (modes.empty()) {
      DD_LOG(error) << "Modes map is empty!";
      return false;
    }

    std::set<std::string> device_ids;
    for (const auto &[device_id, _] : modes) {
      device_ids.insert(device_id);
    }
    const auto all_device_ids {win_utils::getAllDeviceIdsAndMatchingDuplicates(*m_w_api, device_ids)};
    if (all_device_ids.empty()) {
      DD_LOG(warning) << "Failed to get all duplicated devices!";
      return false;
    }

    if (all_device_ids.size() != device_ids.size()) {
      DD_LOG(error) << "Not all modes for duplicate displays were provided!";
      return false;
    }

    const auto &original_data {m_w_api->queryDisplayConfig(QueryType::All)};
    if (!original_data) {
      return false;
    }

    auto current_modes {getCurrentDisplayModes(device_ids)};
    if (current_modes.empty()) {
      return false;
    }

    const auto resolved {resolveRequestedModes(*m_w_api, modes, device_ids, current_modes)};
    if (!resolved) {
      return false;
    }

    if (!resolved->requires_apply) {
      return true;
    }

    const auto apply_and_verify = [&](const DeviceDisplayModeMap &to_apply, const Strategy strategy, const PersistPolicy persist) {
      if (!doSetModes(*m_w_api, to_apply, strategy, persist)) {
        return false;
      }
      const auto applied_modes {getCurrentDisplayModes(device_ids)};
      return !applied_modes.empty() && modesMatch(applied_modes, to_apply);
    };

    if (apply_and_verify(resolved->resolved, Strategy::Relaxed, PersistPolicy::SaveToDatabase)) {
      return true;
    }

    DD_LOG(info) << "Failed to apply resolved display modes with relaxed strategy; retrying strictly.";
    if (apply_and_verify(resolved->resolved, Strategy::Strict, PersistPolicy::SaveToDatabase)) {
      return true;
    }

    const UINT32 flags {SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_VIRTUAL_MODE_AWARE};
    static_cast<void>(m_w_api->setDisplayConfig(original_data->m_paths, original_data->m_modes, flags));
    DD_LOG(error) << "Failed to set display mode(-s) completely!";
    return false;
  }

  bool WinDisplayDevice::setDisplayModesTemporary(const DeviceDisplayModeMap &modes) {
    if (modes.empty()) {
      DD_LOG(error) << "Modes map is empty!";
      return false;
    }

    std::set<std::string> device_ids;
    for (const auto &[device_id, _] : modes) {
      device_ids.insert(device_id);
    }
    const auto all_device_ids {win_utils::getAllDeviceIdsAndMatchingDuplicates(*m_w_api, device_ids)};
    if (all_device_ids.empty() || all_device_ids.size() != device_ids.size()) {
      DD_LOG(error) << "Failed to get all duplicated devices for temporary apply!";
      return false;
    }

    auto current_modes {getCurrentDisplayModes(device_ids)};
    if (current_modes.empty()) {
      return false;
    }

    const auto resolved {resolveRequestedModes(*m_w_api, modes, device_ids, current_modes)};
    if (!resolved) {
      return false;
    }

    if (!resolved->requires_apply) {
      return true;
    }

    const auto apply_and_verify = [&](const Strategy strategy) {
      if (!doSetModes(*m_w_api, resolved->resolved, strategy, PersistPolicy::Temporary)) {
        return false;
      }
      const auto applied_modes {getCurrentDisplayModes(device_ids)};
      return !applied_modes.empty() && modesMatch(applied_modes, resolved->resolved);
    };

    if (apply_and_verify(Strategy::Relaxed)) {
      return true;
    }

    return apply_and_verify(Strategy::Strict);
  }

  bool WinDisplayDevice::setDisplayModesWithFallback(const DeviceDisplayModeMap &modes) {
    if (modes.empty()) {
      DD_LOG(error) << "Modes map is empty!";
      return false;
    }

    if (doSetModes(*m_w_api, modes, Strategy::Relaxed, PersistPolicy::SaveToDatabase)) {
      return true;
    }

    return doSetModes(*m_w_api, modes, Strategy::Strict, PersistPolicy::SaveToDatabase);
  }
}  // namespace display_device
