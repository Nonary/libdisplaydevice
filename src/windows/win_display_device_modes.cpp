/**
 * @file src/windows/win_display_device_modes.cpp
 * @brief Definitions for the display mode related methods in WinDisplayDevice.
 */
// class header include
#include "display_device/windows/win_display_device.h"

// system includes
#include <ranges>
#include <limits>

// local includes
#include "display_device/logging.h"
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
          // Since we are in strict mode, do not fuzzy compare it
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
          // Clear the target index so that Windows has to select/modify the target to best match the requirements.
          win_utils::setTargetIndex(*path, std::nullopt);
          win_utils::setDesktopIndex(*path, std::nullopt);  // Part of struct containing target index and so it needs to be cleared
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
        // It's probably best for Windows to select the "best" display settings for us. However, in case we
        // have custom resolution set in nvidia control panel for example, this flag will prevent successfully applying
        // settings to it.
        flags |= SDC_ALLOW_CHANGES;
      }

      const LONG result {w_api.setDisplayConfig(display_data->m_paths, display_data->m_modes, flags)};
      if (result != ERROR_SUCCESS) {
        DD_LOG(error) << w_api.getErrorString(result) << " failed to set display mode!";
        return false;
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
      // Error already logged
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

      // For whatever reason they put refresh rate into path, but not the resolution.
      const auto target_refresh_rate {path->targetInfo.refreshRate};
      current_modes[device_id] = DisplayMode {
        {source_mode->width, source_mode->height},
        {target_refresh_rate.Numerator, target_refresh_rate.Denominator}
      };
    }

    return current_modes;
  }

  bool WinDisplayDevice::setDisplayModes(const DeviceDisplayModeMap &modes) {
    const auto do_apply = [this](const DeviceDisplayModeMap &to_apply, const Strategy strategy) {
      return doSetModes(*m_w_api, to_apply, strategy, PersistPolicy::SaveToDatabase);
    };

    if (modes.empty()) {
      DD_LOG(error) << "Modes map is empty!";
      return false;
    }

    // Here it is important to check that we have all the necessary modes, otherwise
    // setting modes will fail with ambiguous message.
    //
    // Duplicated devices can have different target modes (monitor) with different refresh rate,
    // however this does not apply to the source mode (frame buffer?) and they must have same
    // resolution.
    //
    // Without SDC_VIRTUAL_MODE_AWARE, devices would share the same source mode entry, but now
    // they have separate entries that are more or less identical.
    //
    // To avoid surprising end-user with unexpected source mode change, we validate that all duplicate
    // devices were provided instead of guessing modes automatically. This also resolve the problem of
    // having to choose refresh rate for duplicate display - leave it to the end-user of this function...
    const auto keys_view {std::ranges::views::keys(modes)};
    const std::set<std::string> device_ids {std::begin(keys_view), std::end(keys_view)};
    const auto all_device_ids {win_utils::getAllDeviceIdsAndMatchingDuplicates(*m_w_api, device_ids)};
    if (all_device_ids.empty()) {
      DD_LOG(error) << "Failed to get all duplicated devices!";
      return false;
    }

    if (all_device_ids.size() != device_ids.size()) {
      DD_LOG(error) << "Not all modes for duplicate displays were provided!";
      return false;
    }

    const auto &original_data {m_w_api->queryDisplayConfig(QueryType::All)};
    if (!original_data) {
      // Error already logged
      return false;
    }

    if (!do_apply(modes, Strategy::Relaxed)) {
      // Error already logged
      return false;
    }

    const auto all_modes_match = [&modes](const DeviceDisplayModeMap &current_modes) {
      for (const auto &[device_id, requested_mode] : modes) {
        auto mode_it {current_modes.find(device_id)};
        if (mode_it == std::end(current_modes)) {
          // This is a sanity check as `getCurrentDisplayModes` implicitly verifies this already.
          return false;
        }

        if (!win_utils::fuzzyCompareModes(mode_it->second, requested_mode)) {
          return false;
        }
      }

      return true;
    };

    auto current_modes {getCurrentDisplayModes(device_ids)};
    if (!current_modes.empty()) {
      if (all_modes_match(current_modes)) {
        return true;
      }

      // We have a problem when using SetDisplayConfig with SDC_ALLOW_CHANGES
      // where it decides to use our new mode merely as a suggestion.
      //
      // This is good, since we don't have to be very precise with refresh rate,
      // but also bad since it can just ignore our specified mode.
      //
      // However, it is possible that the user has created a custom display mode
      // which is not exposed to the via Windows settings app. To allow this
      // resolution to be selected, we actually need to omit SDC_ALLOW_CHANGES
      // flag.
      // First, try a relaxed fallback by enumerating supported modes and choosing the closest
      // mode per display while keeping the aspect ratio. Only if nothing matches the aspect
      // ratio for all displays will we consider other aspect ratios.
      {
        DeviceDisplayModeMap enum_fallback_modes {modes};
        const auto display_active {m_w_api->queryDisplayConfig(QueryType::Active)};
        if (display_active) {
          auto same_aspect = [](const Resolution &a, const Resolution &b) {
            long long lhs = static_cast<long long>(a.m_width) * static_cast<long long>(b.m_height);
            long long rhs = static_cast<long long>(b.m_width) * static_cast<long long>(a.m_height);
            return lhs == rhs;
          };

          const auto refresh_to_double = [](const Rational &r) -> double {
            if (r.m_denominator == 0) return 0.0;
            return static_cast<double>(r.m_numerator) / static_cast<double>(r.m_denominator);
          };

          // Pre-collect supported modes per device
          std::map<std::string, std::vector<DisplayMode>> supported_by_device;
          for (const auto &[device_id, requested_mode] : modes) {
            const auto path {win_utils::getActivePath(*m_w_api, device_id, display_active->m_paths)};
            if (!path) {
              continue;
            }
            auto supported {m_w_api->getSupportedDisplayModes(*path)};
            if (!supported.empty()) {
              supported_by_device.emplace(device_id, std::move(supported));
            }
          }

          // If we failed to enumerate any, skip this fallback
          if (supported_by_device.size() == modes.size()) {
            // Choose best mode per device: first pass picks per-device best with same aspect
            for (const auto &[device_id, requested_mode] : modes) {
              const auto &cands = supported_by_device.at(device_id);
              const auto req_area = static_cast<long long>(requested_mode.m_resolution.m_width) * static_cast<long long>(requested_mode.m_resolution.m_height);
              const double req_hz = refresh_to_double(requested_mode.m_refresh_rate);

              auto best = std::optional<DisplayMode> {};
              auto best_score = std::numeric_limits<long long>::max();
              double best_refresh_delta = std::numeric_limits<double>::infinity();

              // Prefer same aspect
              for (const auto &cand : cands) {
                if (!same_aspect(cand.m_resolution, requested_mode.m_resolution)) continue;
                const auto area = static_cast<long long>(cand.m_resolution.m_width) * static_cast<long long>(cand.m_resolution.m_height);
                const auto area_delta = std::llabs(area - req_area);
                const double cand_hz = refresh_to_double(cand.m_refresh_rate);
                const double hz_delta = std::abs(cand_hz - req_hz);
                if (!best || area_delta < best_score || (area_delta == best_score && hz_delta < best_refresh_delta)) {
                  best = cand;
                  best_score = area_delta;
                  best_refresh_delta = hz_delta;
                }
              }

              if (!best) {
                // Fall back to any aspect ratio if none matched
                for (const auto &cand : cands) {
                  const auto area = static_cast<long long>(cand.m_resolution.m_width) * static_cast<long long>(cand.m_resolution.m_height);
                  const auto area_delta = std::llabs(area - req_area);
                  const double cand_hz = refresh_to_double(cand.m_refresh_rate);
                  const double hz_delta = std::abs(cand_hz - req_hz);
                  if (!best || area_delta < best_score || (area_delta == best_score && hz_delta < best_refresh_delta)) {
                    best = cand;
                    best_score = area_delta;
                    best_refresh_delta = hz_delta;
                  }
                }
              }

              if (best) {
                enum_fallback_modes[device_id] = *best;
              }
            }

            // Apply enumerated fallback and verify
            DD_LOG(info) << "Relaxed apply didn't match; retrying with enumerated-mode fallback.";
            if (do_apply(enum_fallback_modes, Strategy::Relaxed)) {
              const auto current_after_enum {getCurrentDisplayModes(device_ids)};
              const auto all_enum_match = [&enum_fallback_modes](const DeviceDisplayModeMap &cur) {
                for (const auto &[did, exp] : enum_fallback_modes) {
                  auto it = cur.find(did);
                  if (it == std::end(cur)) return false;
                  if (!win_utils::fuzzyCompareModes(it->second, exp)) return false;
                }
                return true;
              };
              if (!current_after_enum.empty() && all_enum_match(current_after_enum)) {
                return true;
              }
            }
          }
        }
      }

      // Try a relaxed fallback constrained by aspect ratio using the display preferred resolution
      DeviceDisplayModeMap aspect_fallback_modes {modes};
      if (const auto display_active {m_w_api->queryDisplayConfig(QueryType::Active)}; display_active) {
        auto same_aspect = [](const Resolution &a, const Resolution &b) {
          // Compare aspect ratios using cross-multiplication to avoid floating point errors
          long long lhs = static_cast<long long>(a.m_width) * static_cast<long long>(b.m_height);
          long long rhs = static_cast<long long>(b.m_width) * static_cast<long long>(a.m_height);
          return lhs == rhs;
        };

        bool adjusted {false};
        for (const auto &[device_id, requested_mode] : modes) {
          const auto path {win_utils::getActivePath(*m_w_api, device_id, display_active->m_paths)};
          if (!path) {
            continue;
          }
          const auto preferred_res {m_w_api->getPreferredResolution(*path)};
          if (preferred_res && same_aspect(*preferred_res, requested_mode.m_resolution)) {
            // Adjust resolution to preferred one, leave refresh rate unchanged to let Windows pick best
            aspect_fallback_modes[device_id].m_resolution = *preferred_res;
            adjusted = true;
          }
        }

        if (adjusted) {
          DD_LOG(info) << "Relaxed apply didn’t match exactly; retrying with preferred-resolution aspect fallback.";
          if (do_apply(aspect_fallback_modes, Strategy::Relaxed)) {
            const auto current_modes_after_fallback {getCurrentDisplayModes(device_ids)};
            // Verify against the expected fallback request
            const auto all_fallback_match = [&aspect_fallback_modes](const DeviceDisplayModeMap &cur) {
              for (const auto &[did, exp] : aspect_fallback_modes) {
                auto it = cur.find(did);
                if (it == std::end(cur)) {
                  return false;
                }
                if (!win_utils::fuzzyCompareModes(it->second, exp)) {
                  return false;
                }
              }
              return true;
            };
            if (!current_modes_after_fallback.empty() && all_fallback_match(current_modes_after_fallback)) {
              return true;
            }
          }
        }
      }

      DD_LOG(info) << "Failed to change display modes with relaxed approach; trying strict.";
      if (do_apply(modes, Strategy::Strict)) {
        current_modes = getCurrentDisplayModes(device_ids);
        if (!current_modes.empty() && all_modes_match(current_modes)) {
          return true;
        }
      }
    }

    const UINT32 flags {SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_VIRTUAL_MODE_AWARE};
    static_cast<void>(m_w_api->setDisplayConfig(original_data->m_paths, original_data->m_modes, flags));  // Return value does not matter as we are trying out best to undo
    DD_LOG(error) << "Failed to set display mode(-s) completely!";
    return false;
  }

  bool WinDisplayDevice::setDisplayModesTemporary(const DeviceDisplayModeMap &modes) {
    const auto do_apply = [this](const DeviceDisplayModeMap &to_apply, const Strategy strategy) {
      return doSetModes(*m_w_api, to_apply, strategy, PersistPolicy::Temporary);
    };

    if (modes.empty()) {
      DD_LOG(error) << "Modes map is empty!";
      return false;
    }

    const auto keys_view {std::ranges::views::keys(modes)};
    const std::set<std::string> device_ids {std::begin(keys_view), std::end(keys_view)};
    const auto all_device_ids {win_utils::getAllDeviceIdsAndMatchingDuplicates(*m_w_api, device_ids)};
    if (all_device_ids.empty()) {
      DD_LOG(error) << "Failed to get all duplicated devices!";
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

    if (!do_apply(modes, Strategy::Relaxed)) {
      return false;
    }

    const auto all_modes_match = [&modes](const DeviceDisplayModeMap &current_modes) {
      for (const auto &[device_id, requested_mode] : modes) {
        auto mode_it {current_modes.find(device_id)};
        if (mode_it == std::end(current_modes)) {
          return false;
        }
        if (!win_utils::fuzzyCompareModes(mode_it->second, requested_mode)) {
          return false;
        }
      }
      return true;
    };

    auto current_modes {getCurrentDisplayModes(device_ids)};
    if (!current_modes.empty()) {
      if (all_modes_match(current_modes)) {
        return true;
      }

      // First, try a relaxed fallback by enumerating supported modes and choosing the closest
      // mode per display while keeping the aspect ratio. Only if nothing matches the aspect
      // ratio for all displays will we consider other aspect ratios.
      {
        DeviceDisplayModeMap enum_fallback_modes {modes};
        const auto display_active {m_w_api->queryDisplayConfig(QueryType::Active)};
        if (display_active) {
          auto same_aspect = [](const Resolution &a, const Resolution &b) {
            long long lhs = static_cast<long long>(a.m_width) * static_cast<long long>(b.m_height);
            long long rhs = static_cast<long long>(b.m_width) * static_cast<long long>(a.m_height);
            return lhs == rhs;
          };

          const auto refresh_to_double = [](const Rational &r) -> double {
            if (r.m_denominator == 0) return 0.0;
            return static_cast<double>(r.m_numerator) / static_cast<double>(r.m_denominator);
          };

          std::map<std::string, std::vector<DisplayMode>> supported_by_device;
          for (const auto &[device_id, requested_mode] : modes) {
            const auto path {win_utils::getActivePath(*m_w_api, device_id, display_active->m_paths)};
            if (!path) continue;
            auto supported {m_w_api->getSupportedDisplayModes(*path)};
            if (!supported.empty()) {
              supported_by_device.emplace(device_id, std::move(supported));
            }
          }

          if (supported_by_device.size() == modes.size()) {
            for (const auto &[device_id, requested_mode] : modes) {
              const auto &cands = supported_by_device.at(device_id);
              const auto req_area = static_cast<long long>(requested_mode.m_resolution.m_width) * static_cast<long long>(requested_mode.m_resolution.m_height);
              const double req_hz = refresh_to_double(requested_mode.m_refresh_rate);

              auto best = std::optional<DisplayMode> {};
              auto best_score = std::numeric_limits<long long>::max();
              double best_refresh_delta = std::numeric_limits<double>::infinity();

              for (const auto &cand : cands) {
                if (!same_aspect(cand.m_resolution, requested_mode.m_resolution)) continue;
                const auto area = static_cast<long long>(cand.m_resolution.m_width) * static_cast<long long>(cand.m_resolution.m_height);
                const auto area_delta = std::llabs(area - req_area);
                const double cand_hz = refresh_to_double(cand.m_refresh_rate);
                const double hz_delta = std::abs(cand_hz - req_hz);
                if (!best || area_delta < best_score || (area_delta == best_score && hz_delta < best_refresh_delta)) {
                  best = cand;
                  best_score = area_delta;
                  best_refresh_delta = hz_delta;
                }
              }

              if (!best) {
                for (const auto &cand : cands) {
                  const auto area = static_cast<long long>(cand.m_resolution.m_width) * static_cast<long long>(cand.m_resolution.m_height);
                  const auto area_delta = std::llabs(area - req_area);
                  const double cand_hz = refresh_to_double(cand.m_refresh_rate);
                  const double hz_delta = std::abs(cand_hz - req_hz);
                  if (!best || area_delta < best_score || (area_delta == best_score && hz_delta < best_refresh_delta)) {
                    best = cand;
                    best_score = area_delta;
                    best_refresh_delta = hz_delta;
                  }
                }
              }

              if (best) {
                enum_fallback_modes[device_id] = *best;
              }
            }

            DD_LOG(info) << "Relaxed temporary apply didn't match; retrying with enumerated-mode fallback (strict).";
            if (do_apply(enum_fallback_modes, Strategy::Strict)) {
              const auto current_after_enum {getCurrentDisplayModes(device_ids)};
              const auto all_enum_match = [&enum_fallback_modes](const DeviceDisplayModeMap &cur) {
                for (const auto &[did, exp] : enum_fallback_modes) {
                  auto it = cur.find(did);
                  if (it == std::end(cur)) return false;
                  if (!win_utils::fuzzyCompareModes(it->second, exp)) return false;
                }
                return true;
              };
              if (!current_after_enum.empty() && all_enum_match(current_after_enum)) {
                return true;
              }
            }
          }
        }
      }

      // Aspect-ratio constrained relaxed fallback
      DeviceDisplayModeMap aspect_fallback_modes {modes};
      if (const auto display_active {m_w_api->queryDisplayConfig(QueryType::Active)}; display_active) {
        auto same_aspect = [](const Resolution &a, const Resolution &b) {
          long long lhs = static_cast<long long>(a.m_width) * static_cast<long long>(b.m_height);
          long long rhs = static_cast<long long>(b.m_width) * static_cast<long long>(a.m_height);
          return lhs == rhs;
        };

        bool adjusted {false};
        for (const auto &[device_id, requested_mode] : modes) {
          const auto path {win_utils::getActivePath(*m_w_api, device_id, display_active->m_paths)};
          if (!path) {
            continue;
          }
          const auto preferred_res {m_w_api->getPreferredResolution(*path)};
          if (preferred_res && same_aspect(*preferred_res, requested_mode.m_resolution)) {
            aspect_fallback_modes[device_id].m_resolution = *preferred_res;
            adjusted = true;
          }
        }

        if (adjusted) {
          DD_LOG(info) << "Relaxed temporary apply didn't match; retrying with preferred-resolution aspect fallback (strict).";
          if (do_apply(aspect_fallback_modes, Strategy::Strict)) {
            const auto current_modes_after_fallback {getCurrentDisplayModes(device_ids)};
            const auto all_fallback_match = [&aspect_fallback_modes](const DeviceDisplayModeMap &cur) {
              for (const auto &[did, exp] : aspect_fallback_modes) {
                auto it = cur.find(did);
                if (it == std::end(cur)) {
                  return false;
                }
                if (!win_utils::fuzzyCompareModes(it->second, exp)) {
                  return false;
                }
              }
              return true;
            };
            if (!current_modes_after_fallback.empty() && all_fallback_match(current_modes_after_fallback)) {
              return true;
            }
          }
        }
      }

      if (do_apply(modes, Strategy::Strict)) {
        current_modes = getCurrentDisplayModes(device_ids);
        if (!current_modes.empty() && all_modes_match(current_modes)) {
          return true;
        }
      }
    }

    // Undo on failure (temporary, no save to database)
    const UINT32 flags {SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_NO_OPTIMIZATION | SDC_VIRTUAL_MODE_AWARE};
    static_cast<void>(m_w_api->setDisplayConfig(original_data->m_paths, original_data->m_modes, flags));
    DD_LOG(error) << "Failed to set display mode(-s) completely (temporary apply)!";
    return false;
  }
}  // namespace display_device
