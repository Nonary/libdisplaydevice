/**
 * @file src/windows/settings_manager_apply.cpp
 * @brief Definitions for the methods for applying settings in SettingsManager.
 */
// class header include
#include "display_device/windows/settings_manager.h"

// system includes
#include <algorithm>
#include <set>
#include <cmath>
#include <limits>
#include <boost/scope/scope_exit.hpp>

// Windows headers (for display mode enumeration)
#define NOMINMAX
#include <windows.h>

// local includes
#include "display_device/logging.h"
#include "display_device/windows/json.h"
#include "display_device/windows/settings_utils.h"
#include "display_device/windows/win_api_utils.h"

namespace display_device {
  namespace {
    /**
     * @brief Function that does nothing.
     */
    void noopFn() {
    }

    // --- Aspect/Distance helpers (prefer same-AR, then closest resolution) ---
    static inline bool sameAspectFuzzy(unsigned int tw, unsigned int th,
                                       unsigned int w,  unsigned int h,
                                       double eps = 1e-3) {
      // Use cross-multiplication in long double to avoid precision loss.
      const long double lhs = static_cast<long double>(tw) * static_cast<long double>(h);
      const long double rhs = static_cast<long double>(w)  * static_cast<long double>(th);
      const long double denom = std::max<long double>(1.0L, static_cast<long double>(tw) * th);
      return std::fabsl(lhs - rhs) <= eps * denom;
    }

    static inline bool isDownscale(unsigned int tw, unsigned int th,
                                   unsigned int w,  unsigned int h) {
      return (w <= tw && h <= th);
    }

    static inline unsigned long long dist2(unsigned int tw, unsigned int th,
                                           unsigned int w,  unsigned int h) {
      const long long dx = static_cast<long long>(w) - static_cast<long long>(tw);
      const long long dy = static_cast<long long>(h) - static_cast<long long>(th);
      // No need to sqrt; squared distance preserves ordering.
      return static_cast<unsigned long long>(dx * dx + dy * dy);
    }

    struct SupportedMode {
      unsigned int w;
      unsigned int h;
      Rational rr;
      bool interlaced;
      bool preferred;
    };

    static std::optional<SupportedMode> devmodeToMode(const DEVMODEW &dm, bool preferred) {
      if ((dm.dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) != (DM_PELSWIDTH | DM_PELSHEIGHT)) {
        return std::nullopt;
      }
      const unsigned int w = static_cast<unsigned int>(dm.dmPelsWidth);
      const unsigned int h = static_cast<unsigned int>(dm.dmPelsHeight);
      if (w == 0 || h == 0) return std::nullopt;

      unsigned int hz = 0;
      if (dm.dmFields & DM_DISPLAYFREQUENCY) {
        hz = static_cast<unsigned int>(dm.dmDisplayFrequency);
      }
      const bool interlaced = (dm.dmDisplayFlags & DM_INTERLACED) != 0;
      return SupportedMode {w, h, Rational {hz, 1}, interlaced, preferred};
    }

    static std::vector<SupportedMode> enumerateSupportedModes(const std::wstring &display_name) {
      std::vector<SupportedMode> out;
      if (display_name.empty()) return out;

      // Prefer registry/current as preferred
      for (DWORD which : {ENUM_REGISTRY_SETTINGS, ENUM_CURRENT_SETTINGS}) {
        DEVMODEW dm {0}; dm.dmSize = sizeof(dm);
        if (::EnumDisplaySettingsExW(display_name.c_str(), which, &dm, 0)) {
          if (auto m = devmodeToMode(dm, true)) out.push_back(*m);
        }
      }

      for (DWORD i = 0;; ++i) {
        DEVMODEW dm {0}; dm.dmSize = sizeof(dm);
        if (!::EnumDisplaySettingsExW(display_name.c_str(), i, &dm, 0)) break;
        if (auto m = devmodeToMode(dm, false)) out.push_back(*m);
      }

      if (out.empty()) return out;

      // Deduplicate by resolution and approx rr bucket (~0.05Hz). Keep progressive and highest rr within each bucket.
      struct Key { unsigned int w; unsigned int h; double rr; };
      auto rr_bucket = [](const SupportedMode &m){
        const double v = (m.rr.m_denominator == 0) ? 0.0 : static_cast<double>(m.rr.m_numerator) / static_cast<double>(m.rr.m_denominator);
        return std::round(v * 20.0) / 20.0;
      };

      std::map<std::pair<unsigned,int>, std::map<double, SupportedMode>> grouped; // pair<w,h> -> bucket -> best mode
      for (const auto &m : out) {
        const auto res_key = std::make_pair(static_cast<unsigned>(m.w), static_cast<int>(m.h));
        const double b = rr_bucket(m);
        auto &by_bucket = grouped[res_key];
        auto it = by_bucket.find(b);
        if (it == by_bucket.end()) {
          by_bucket.emplace(b, m);
        } else {
          const auto &cur = it->second;
          const bool better_progressive = cur.interlaced && !m.interlaced;
          const bool better_rr = (static_cast<long long>(m.rr.m_numerator) * cur.rr.m_denominator) > (static_cast<long long>(cur.rr.m_numerator) * m.rr.m_denominator);
          if (better_progressive || (!better_progressive && better_rr)) it->second = m;
        }
      }

      std::vector<SupportedMode> deduped;
      for (const auto &rk : grouped) {
        for (const auto &bk : rk.second) deduped.push_back(bk.second);
      }

      std::sort(deduped.begin(), deduped.end(), [](const SupportedMode &a, const SupportedMode &b){
        const unsigned long long area_a = static_cast<unsigned long long>(a.w) * a.h;
        const unsigned long long area_b = static_cast<unsigned long long>(b.w) * b.h;
        if (area_a != area_b) return area_a > area_b;
        const double ar_a = static_cast<double>(a.w) / static_cast<double>(a.h);
        const double ar_b = static_cast<double>(b.w) / static_cast<double>(b.h);
        const double d_a = std::abs(ar_a - (16.0/9.0));
        const double d_b = std::abs(ar_b - (16.0/9.0));
        if (d_a != d_b) return d_a < d_b;
        const long long lhs = static_cast<long long>(a.rr.m_numerator) * b.rr.m_denominator;
        const long long rhs = static_cast<long long>(b.rr.m_numerator) * a.rr.m_denominator;
        if (lhs != rhs) return lhs > rhs;
        if (a.interlaced != b.interlaced) return !a.interlaced && b.interlaced;
        return a.preferred && !b.preferred;
      });

      return deduped;
    }

    static double scoreAspectArea(unsigned int tw, unsigned int th, unsigned int cw, unsigned int ch) {
      const double ta = static_cast<double>(tw) / static_cast<double>(th);
      const double ca = static_cast<double>(cw) / static_cast<double>(ch);
      const double ar_score = std::abs(ta - ca);
      const double area_t = static_cast<double>(tw) * th;
      const double area_c = static_cast<double>(cw) * ch;
      const double area_score = std::abs(area_t - area_c) / std::max(1.0, area_t);
      return ar_score * 1.0 + area_score * 0.5;
    }

    static std::optional<SupportedMode> pickBestMode(const std::vector<SupportedMode> &modes,
                                                     const std::optional<Resolution> &want_res,
                                                     const std::optional<FloatingPoint> &want_rr,
                                                     const Resolution &current_res) {
      if (modes.empty()) return std::nullopt;
      auto rrEquals = [](const Rational &a, const Rational &b) { return win_utils::fuzzyCompareRefreshRates(a, b); };
      auto rrFromFloating = [](const FloatingPoint &value) -> Rational {
        if (const auto *r = std::get_if<Rational>(&value)) return *r;
        constexpr unsigned den = 10000; return Rational {static_cast<unsigned>(std::round(std::get<double>(value) * den)), den};
      };
      const std::optional<Rational> want_rr_r = want_rr ? std::optional<Rational> { rrFromFloating(*want_rr) } : std::nullopt;
      auto above_floor = [](unsigned int w, unsigned int h) {
        const bool above_720p = (w >= 1280 && h >= 720);
        const bool above_xga  = (w >= 1024 && h >= 768);
        return above_720p || above_xga;
      };

      std::vector<SupportedMode> by_res;
      if (want_res) {
        for (const auto &m : modes) if (m.w == want_res->m_width && m.h == want_res->m_height) by_res.push_back(m);
      }

      if (want_res && want_rr_r) {
        for (const auto &m : by_res) if (rrEquals(m.rr, *want_rr_r)) return m;
        if (!by_res.empty()) {
          auto better = [&](const SupportedMode &lhs, const SupportedMode &rhs) {
            const long long dl = static_cast<long long>(lhs.rr.m_numerator) * want_rr_r->m_denominator - static_cast<long long>(want_rr_r->m_numerator) * lhs.rr.m_denominator;
            const long long dr = static_cast<long long>(rhs.rr.m_numerator) * want_rr_r->m_denominator - static_cast<long long>(want_rr_r->m_numerator) * rhs.rr.m_denominator;
            const bool lhs_ge = dl >= 0, rhs_ge = dr >= 0; if (lhs_ge != rhs_ge) return lhs_ge; // prefer >= requested
            const auto abl = std::llabs(dl), abr = std::llabs(dr); if (abl != abr) return abl < abr;
            if (lhs.interlaced != rhs.interlaced) return !lhs.interlaced && rhs.interlaced;
            return (static_cast<long long>(lhs.rr.m_numerator) * rhs.rr.m_denominator) > (static_cast<long long>(rhs.rr.m_numerator) * lhs.rr.m_denominator);
          };
          return *std::min_element(by_res.begin(), by_res.end(), [&](const auto &a, const auto &b){ return !better(b, a); });
        }
      }

      if (want_res && !want_rr_r) {
        if (!by_res.empty()) {
          return *std::max_element(by_res.begin(), by_res.end(), [](const auto &a, const auto &b){
            if (a.interlaced != b.interlaced) return a.interlaced && !b.interlaced; // progressive first
            return (static_cast<long long>(a.rr.m_numerator) * b.rr.m_denominator) < (static_cast<long long>(b.rr.m_numerator) * a.rr.m_denominator);
          });
        }
      }

      if (!want_res && want_rr_r) {
        std::vector<SupportedMode> same_res;
        for (const auto &m : modes) if (m.w == current_res.m_width && m.h == current_res.m_height) same_res.push_back(m);
        if (!same_res.empty()) {
          return pickBestMode(same_res, Resolution {current_res.m_width, current_res.m_height}, want_rr, current_res);
        }
      }

      // Prefer same-aspect, closest resolution when a desired resolution was specified.
      if (want_res) {
        std::optional<SupportedMode> best;
        unsigned long long best_d2 = std::numeric_limits<unsigned long long>::max();
        bool best_down = false;

        auto rr_better = [&](const SupportedMode &lhs, const SupportedMode &rhs) {
          if (want_rr_r) {
            const long long dl = static_cast<long long>(lhs.rr.m_numerator) * want_rr_r->m_denominator
                               - static_cast<long long>(want_rr_r->m_numerator) * lhs.rr.m_denominator;
            const long long dr = static_cast<long long>(rhs.rr.m_numerator) * want_rr_r->m_denominator
                               - static_cast<long long>(want_rr_r->m_numerator) * rhs.rr.m_denominator;
            const bool lhs_ge = dl >= 0, rhs_ge = dr >= 0; // prefer >= requested
            if (lhs_ge != rhs_ge) return lhs_ge;
            const auto abl = std::llabs(dl), abr = std::llabs(dr);
            if (abl != abr) return abl < abr; // closer to requested RR
          }
          if (lhs.interlaced != rhs.interlaced) return !lhs.interlaced && rhs.interlaced; // progressive first
          // higher RR
          return (static_cast<long long>(lhs.rr.m_numerator) * rhs.rr.m_denominator)
               > (static_cast<long long>(rhs.rr.m_numerator) * lhs.rr.m_denominator);
        };

        for (const auto &m : modes) {
          if (!above_floor(m.w, m.h)) continue;
          if (!sameAspectFuzzy(want_res->m_width, want_res->m_height, m.w, m.h)) continue;
          const auto d2 = dist2(want_res->m_width, want_res->m_height, m.w, m.h);
          const bool down = isDownscale(want_res->m_width, want_res->m_height, m.w, m.h);
          if (!best || d2 < best_d2 ||
              (d2 == best_d2 && down && !best_down) ||
              (d2 == best_d2 && down == best_down && rr_better(m, *best))) {
            best = m; best_d2 = d2; best_down = down;
          }
        }
        if (best) return best;
      }

      // Fallback: previous heuristic if no same-AR candidates exist
      const unsigned int target_w = want_res ? want_res->m_width : current_res.m_width;
      const unsigned int target_h = want_res ? want_res->m_height : current_res.m_height;
      double best_score = 1e9; std::optional<SupportedMode> best;
      for (const auto &m : modes) {
        if (!above_floor(m.w, m.h) && (!want_res || !(m.w == want_res->m_width && m.h == want_res->m_height))) continue;
        const double s = scoreAspectArea(target_w, target_h, m.w, m.h);
        if (!best || s < best_score) { best = m; best_score = s; }
        else if (s == best_score) {
          const bool better_progressive = best->interlaced && !m.interlaced;
          const bool better_rr = (static_cast<long long>(m.rr.m_numerator) * best->rr.m_denominator)
                               > (static_cast<long long>(best->rr.m_numerator) * m.rr.m_denominator);
          if (better_progressive || (!better_progressive && better_rr)) best = m;
        }
      }
      return best;
    }
  }  // namespace

  SettingsManager::ApplyResult SettingsManager::applySettings(const SingleDisplayConfiguration &config) {
    const auto api_access {m_dd_api->isApiAccessAvailable()};
    DD_LOG(info) << "Trying to apply display device settings. API is available: " << toJson(api_access);

    if (!api_access) {
      return ApplyResult::ApiTemporarilyUnavailable;
    }
    DD_LOG(info) << "Using the following configuration:\n"
                 << toJson(config);

    const auto topology_before_changes {m_dd_api->getCurrentTopology()};
    if (!m_dd_api->isTopologyValid(topology_before_changes)) {
      DD_LOG(error) << "Retrieved current topology is invalid:\n"
                    << toJson(topology_before_changes);
      return ApplyResult::DevicePrepFailed;
    }
    DD_LOG(info) << "Active topology before any changes:\n"
                 << toJson(topology_before_changes);

    bool system_settings_touched {false};
    boost::scope::scope_exit hdr_blank_always_executed_guard {[this, &system_settings_touched]() {
      if (system_settings_touched) {
        win_utils::blankHdrStates(*m_dd_api, m_workarounds.m_hdr_blank_delay);
      }
    }};

    bool release_context {false};
    boost::scope::scope_exit topology_prep_guard {[this, topology = topology_before_changes, was_captured = m_audio_context_api->isCaptured(), &release_context]() {
      // It is possible that during topology preparation, some settings will be reverted for the modified topology.
      // To keel it simple, these settings will not be restored!
      const auto result {m_dd_api->setTopology(topology)};
      if (!result) {
        DD_LOG(error) << "Failed to revert back to topology in the topology guard!";
        if (release_context) {
          // We are currently in the topology for which the context was captured.
          // We have also failed to revert back to some previous one, so we remain in this topology for
          // which we have context. There is no reason to keep it around then...
          m_audio_context_api->release();
        }
      }

      if (!was_captured && m_audio_context_api->isCaptured()) {
        // We only want to release context that was not captured before.
        m_audio_context_api->release();
      }
    }};

    const auto &prepped_topology_data {prepareTopology(config, topology_before_changes, release_context, system_settings_touched)};
    if (!prepped_topology_data) {
      // Error already logged
      return ApplyResult::DevicePrepFailed;
    }
    auto [new_state, device_to_configure, additional_devices_to_configure] = *prepped_topology_data;

    DdGuardFn primary_guard_fn {noopFn};
    boost::scope::scope_exit<DdGuardFn &> primary_guard {primary_guard_fn};
    if (!preparePrimaryDevice(config, device_to_configure, primary_guard_fn, new_state, system_settings_touched)) {
      // Error already logged
      return ApplyResult::PrimaryDevicePrepFailed;
    }

    DdGuardFn mode_guard_fn {noopFn};
    boost::scope::scope_exit<DdGuardFn &> mode_guard {mode_guard_fn};
    if (!prepareDisplayModes(config, device_to_configure, additional_devices_to_configure, mode_guard_fn, new_state, system_settings_touched)) {
      // Error already logged
      return ApplyResult::DisplayModePrepFailed;
    }

    DdGuardFn hdr_state_guard_fn {noopFn};
    boost::scope::scope_exit<DdGuardFn &> hdr_state_guard {hdr_state_guard_fn};
    if (!prepareHdrStates(config, device_to_configure, additional_devices_to_configure, hdr_state_guard_fn, new_state, system_settings_touched)) {
      // Error already logged
      return ApplyResult::HdrStatePrepFailed;
    }

    // We will always keep the new state persistently, even if there are no new meaningful changes, because
    // we want to preserve the initial state for consistency.
    if (!m_persistence_state->persistState(new_state)) {
      DD_LOG(error) << "Failed to save reverted settings! Undoing everything...";
      return ApplyResult::PersistenceSaveFailed;
    }

    // We can only release the context now as nothing else can fail.
    if (release_context) {
      m_audio_context_api->release();
    }

    // Disable all guards before returning
    topology_prep_guard.set_active(false);
    primary_guard.set_active(false);
    mode_guard.set_active(false);
    hdr_state_guard.set_active(false);
    return ApplyResult::Ok;
  }

  std::optional<std::tuple<SingleDisplayConfigState, std::string, std::set<std::string>>> SettingsManager::prepareTopology(const SingleDisplayConfiguration &config, const ActiveTopology &topology_before_changes, bool &release_context, bool &system_settings_touched) {
    const EnumeratedDeviceList devices {m_dd_api->enumAvailableDevices()};
    if (devices.empty()) {
      DD_LOG(error) << "Failed to enumerate display devices!";
      return std::nullopt;
    }
    DD_LOG(info) << "Currently available devices:\n"
                 << toJson(devices);

    if (!config.m_device_id.empty()) {
      auto device_it {std::ranges::find_if(devices, [device_id = config.m_device_id](const auto &item) {
        return item.m_device_id == device_id;
      })};
      if (device_it == std::end(devices)) {
        // Do not use toJson in case the user entered some BS string...
        DD_LOG(error) << "Device \"" << config.m_device_id << "\" is not available in the system!";
        return std::nullopt;
      }
    }

    const auto &cached_state {m_persistence_state->getState()};
    const auto new_initial_state {win_utils::computeInitialState(cached_state ? std::make_optional(cached_state->m_initial) : std::nullopt, topology_before_changes, devices)};
    if (!new_initial_state) {
      // Error already logged
      return std::nullopt;
    }
    SingleDisplayConfigState new_state {*new_initial_state};

    // In case some devices are no longer available in the system, we could try to strip them from the initial state
    // and hope that we are still "safe" to make further changes (to be determined by computeNewTopologyAndMetadata call below).
    const auto stripped_initial_state {win_utils::stripInitialState(new_state.m_initial, devices)};
    if (!stripped_initial_state) {
      // Error already logged
      return std::nullopt;
    }

    // Enforce topology when user is changing modes/HDR without specifying a device (primary group implied).
    const bool changing_settings {config.m_resolution || config.m_refresh_rate || config.m_hdr_state};
    const bool primary_group_implied {config.m_device_id.empty()};
    const auto effective_prep = (changing_settings && primary_group_implied) ? SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay : config.m_device_prep;

    const auto &[new_topology, device_to_configure, additional_devices_to_configure] = win_utils::computeNewTopologyAndMetadata(effective_prep, config.m_device_id, *stripped_initial_state);
    const auto change_is_needed {!m_dd_api->isTopologyTheSame(topology_before_changes, new_topology)};
    DD_LOG(info) << "Newly computed display device topology data:\n"
                 << "  - topology: " << toJson(new_topology, JSON_COMPACT) << "\n"
                 << "  - change is needed: " << toJson(change_is_needed, JSON_COMPACT) << "\n"
                 << "  - additional devices to configure: " << toJson(additional_devices_to_configure, JSON_COMPACT);

    // This check is mainly to cover the case for "config.device_prep == VerifyOnly" as we at least
    // have to validate that the device exists, but it doesn't hurt to double-check it in all cases.
    if (!win_utils::flattenTopology(new_topology).contains(device_to_configure)) {
      DD_LOG(error) << "Device " << toJson(device_to_configure, JSON_COMPACT) << " is not active!";
      return std::nullopt;
    }

    if (change_is_needed) {
      if (cached_state && !m_dd_api->isTopologyTheSame(cached_state->m_modified.m_topology, new_topology)) {
        DD_LOG(warning) << "To apply new display device settings, previous modifications must be undone! Trying to undo them now.";
        if (revertModifiedSettings(topology_before_changes, system_settings_touched) != RevertResult::Ok) {
          DD_LOG(error) << "Failed to apply new configuration, because the previous settings could not be reverted!";
          return std::nullopt;
        }
      }

      const bool audio_is_captured {m_audio_context_api->isCaptured()};
      if (!audio_is_captured) {
        // Non-stripped initial state MUST be checked here as the missing device could have its context captured!
        const bool switching_from_initial {m_dd_api->isTopologyTheSame(new_state.m_initial.m_topology, topology_before_changes)};
        const bool new_topology_contains_all_current_topology_devices {std::ranges::includes(win_utils::flattenTopology(new_topology), win_utils::flattenTopology(topology_before_changes))};
        if (switching_from_initial && !new_topology_contains_all_current_topology_devices) {
          // Only capture the context when switching from initial topology. All the other intermediate states, like non-existent
          // capture state after system restart are to be avoided.
          if (!m_audio_context_api->capture()) {
            DD_LOG(error) << "Failed to capture audio context!";
            return std::nullopt;
          }
        }
      }

      if (!m_dd_api->setTopology(new_topology)) {
        DD_LOG(error) << "Failed to apply new configuration, because a new topology could not be set!";
        return std::nullopt;
      }
      system_settings_touched = true;

      // We can release the context later on if everything is successful as we are switching back to the non-stripped initial state.
      release_context = m_dd_api->isTopologyTheSame(new_state.m_initial.m_topology, new_topology) && audio_is_captured;
    }

    new_state.m_modified.m_topology = new_topology;
    return std::make_tuple(new_state, device_to_configure, additional_devices_to_configure);
  }

  bool SettingsManager::preparePrimaryDevice(const SingleDisplayConfiguration &config, const std::string &device_to_configure, DdGuardFn &guard_fn, SingleDisplayConfigState &new_state, bool &system_settings_touched) {
    const auto &cached_state {m_persistence_state->getState()};
    const auto cached_primary_device {cached_state ? cached_state->m_modified.m_original_primary_device : std::string {}};
    // Also ensure primary when user requested the device to be the only active display.
    // In practice, users expect the targeted monitor to become primary in this mode as well.
    const bool ensure_primary {config.m_device_prep == SingleDisplayConfiguration::DevicePreparation::EnsurePrimary ||
                               config.m_device_prep == SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay};
    const bool might_need_to_restore {!cached_primary_device.empty()};

    std::string current_primary_device;
    if (ensure_primary || might_need_to_restore) {
      current_primary_device = win_utils::getPrimaryDevice(*m_dd_api, new_state.m_modified.m_topology);
      if (current_primary_device.empty()) {
        DD_LOG(error) << "Failed to get primary device for the topology! Searched topology:\n"
                      << toJson(new_state.m_modified.m_topology);
        return false;
      }
    }

    const auto try_change {[&](const std::string &new_device, const auto info_preamble, const auto error_log) {
      if (current_primary_device != new_device) {
        DD_LOG(info) << info_preamble << toJson(new_device);
        if (!m_dd_api->setAsPrimary(new_device)) {
          DD_LOG(error) << error_log;
          return false;
        }

        // Only mark as touched if primary actually changed
        const auto after_primary_device = win_utils::getPrimaryDevice(*m_dd_api, new_state.m_modified.m_topology);
        if (!after_primary_device.empty() && after_primary_device != current_primary_device) {
          system_settings_touched = true;
          guard_fn = win_utils::primaryGuardFn(*m_dd_api, current_primary_device);
        }
      }

      return true;
    }};

    if (ensure_primary) {
      const auto original_primary_device {cached_primary_device.empty() ? current_primary_device : cached_primary_device};
      const auto &new_primary_device {device_to_configure};

      if (!try_change(new_primary_device, "Changing primary display to:\n", "Failed to apply new configuration, because a new primary device could not be set!")) {
        // Error already logged
        return false;
      }

      // Here we preserve the data from persistence (unless there's none) as in the end that is what we want to go back to.
      new_state.m_modified.m_original_primary_device = original_primary_device;
      return true;
    }

    if (might_need_to_restore) {
      if (!try_change(cached_primary_device, "Changing primary display back to:\n", "Failed to restore original primary device!")) {
        // Error already logged
        return false;
      }
    }

    return true;
  }

  bool SettingsManager::prepareDisplayModes(const SingleDisplayConfiguration &config, const std::string &device_to_configure, const std::set<std::string> &additional_devices_to_configure, DdGuardFn &guard_fn, SingleDisplayConfigState &new_state, bool &system_settings_touched) {
    const auto &cached_state {m_persistence_state->getState()};
    const auto cached_display_modes {cached_state ? cached_state->m_modified.m_original_modes : DeviceDisplayModeMap {}};
    const bool change_required {config.m_resolution || config.m_refresh_rate};
    const bool might_need_to_restore {!cached_display_modes.empty()};

    DeviceDisplayModeMap current_display_modes;
    if (change_required || might_need_to_restore) {
      current_display_modes = m_dd_api->getCurrentDisplayModes(win_utils::flattenTopology(new_state.m_modified.m_topology));
      if (current_display_modes.empty()) {
        DD_LOG(error) << "Failed to get current display modes!";
        return false;
      }
    }

    const auto try_change {[&](const DeviceDisplayModeMap &new_modes, const auto info_preamble, const auto error_log) {
      if (current_display_modes != new_modes) {
        DD_LOG(info) << info_preamble << toJson(new_modes);
        if (!m_dd_api->setDisplayModes(new_modes)) {
          DD_LOG(error) << error_log;
          return false;
        }

        // It is possible that the display modes will not actually change even though the "current != new" condition is true.
        // This is because of some additional internal checks that determine whether the change is actually needed.
        // Therefore we should check the current display modes after the fact!
        if (current_display_modes != m_dd_api->getCurrentDisplayModes(win_utils::flattenTopology(new_state.m_modified.m_topology))) {
          system_settings_touched = true;
          guard_fn = win_utils::modeGuardFn(*m_dd_api, current_display_modes);
        }
      }

      return true;
    }};

    if (change_required) {
      const bool configuring_primary_devices {config.m_device_id.empty()};
      const auto original_display_modes {cached_display_modes.empty() ? current_display_modes : cached_display_modes};
      // Capability-based selection: use OS-reported modes per device to pick a deterministic best candidate.
      std::vector<std::string> targets; targets.reserve(1 + additional_devices_to_configure.size());
      targets.push_back(device_to_configure);
      for (const auto &id : additional_devices_to_configure) targets.push_back(id);

      // Gather supported modes and compute intersection of resolutions across targets
      struct PerDeviceCaps { std::string id; std::vector<SupportedMode> modes; std::set<std::pair<unsigned int,unsigned int>> resolutions; };
      std::vector<PerDeviceCaps> caps; caps.reserve(targets.size());
      for (const auto &id : targets) {
        const auto disp_name_str = m_dd_api->getDisplayName(id);
        const std::wstring disp_name(disp_name_str.begin(), disp_name_str.end());
        auto modes = enumerateSupportedModes(disp_name);
        std::set<std::pair<unsigned int,unsigned int>> res_set;
        for (const auto &m : modes) res_set.insert({m.w, m.h});
        caps.push_back(PerDeviceCaps{id, std::move(modes), std::move(res_set)});
      }

      auto intersect_res = [&caps]() {
        std::set<std::pair<unsigned int,unsigned int>> inter;
        if (caps.empty()) return inter;
        inter = caps.front().resolutions;
        for (size_t i = 1; i < caps.size(); ++i) {
          std::set<std::pair<unsigned int,unsigned int>> tmp;
          std::ranges::set_intersection(inter, caps[i].resolutions, std::inserter(tmp, std::begin(tmp)));
          inter.swap(tmp);
        }
        return inter;
      }();

      // Choose a common resolution
      std::optional<Resolution> chosen_res;
      if (config.m_resolution) {
        if (intersect_res.contains({config.m_resolution->m_width, config.m_resolution->m_height})) {
          chosen_res = config.m_resolution;
        }
      }
      if (!chosen_res) {
        // First, strictly prefer same-AR candidates and pick the closest resolution (prefer downscale on ties).
        const auto target_res = config.m_resolution.value_or(original_display_modes.at(device_to_configure).m_resolution);
        unsigned long long best_d2 = std::numeric_limits<unsigned long long>::max();
        std::pair<unsigned int, unsigned int> best_pair {0, 0};
        bool best_down = false;

        for (const auto &p : intersect_res) {
          const unsigned int w = p.first, h = p.second;
          const bool above_720p = (w >= 1280 && h >= 720);
          const bool above_xga  = (w >= 1024 && h >= 768);
          if (!(above_720p || above_xga)) continue;
          if (!sameAspectFuzzy(target_res.m_width, target_res.m_height, w, h)) continue;
          const auto d2 = dist2(target_res.m_width, target_res.m_height, w, h);
          const bool down = isDownscale(target_res.m_width, target_res.m_height, w, h);
          if (d2 < best_d2 || (d2 == best_d2 && down && !best_down)) {
            best_d2 = d2; best_pair = p; best_down = down;
          }
        }
        if (best_pair.first != 0) {
          chosen_res = Resolution{best_pair.first, best_pair.second};
        } else {
          // No same-AR common res: fall back to the previous area/aspect heuristic.
          double best_s = 1e9; std::pair<unsigned int,unsigned int> best_any {0,0};
          for (const auto &p : intersect_res) {
            const unsigned int w = p.first, h = p.second;
            const bool above_720p = (w >= 1280 && h >= 720);
            const bool above_xga  = (w >= 1024 && h >= 768);
            if (!(above_720p || above_xga)) continue;
            const double s = scoreAspectArea(target_res.m_width, target_res.m_height, w, h);
            if (s < best_s) { best_s = s; best_any = p; }
          }
          if (best_any.first != 0) chosen_res = Resolution{best_any.first, best_any.second};
        }
      }

      DeviceDisplayModeMap candidate = original_display_modes;
      bool any_candidate {false};
      if (chosen_res) {
        // Pick per-device refresh for the chosen resolution
        for (const auto &c : caps) {
          // Build a filtered list of modes at chosen resolution for this device
          std::vector<SupportedMode> at_res; at_res.reserve(c.modes.size());
          for (const auto &m : c.modes) if (m.w == chosen_res->m_width && m.h == chosen_res->m_height) at_res.push_back(m);
          const auto &cur = original_display_modes.at(c.id);
          const auto rr_pick = pickBestMode(at_res, chosen_res, config.m_refresh_rate, cur.m_resolution);
          if (rr_pick) {
            candidate[c.id].m_resolution = {rr_pick->w, rr_pick->h};
            candidate[c.id].m_refresh_rate = rr_pick->rr;
            any_candidate = true;
          }
        }
      } else {
        // If no common resolution could be found, try per-device bests (may fail, but better than blind tries)
        for (const auto &c : caps) {
          const auto &cur = original_display_modes.at(c.id);
          const auto pick = pickBestMode(c.modes, config.m_resolution, config.m_refresh_rate, cur.m_resolution);
          if (pick) {
            candidate[c.id].m_resolution = {pick->w, pick->h};
            candidate[c.id].m_refresh_rate = pick->rr;
            any_candidate = true;
          }
        }
      }

      if (any_candidate && try_change(candidate, "Changing display modes (capability-based) to:\n", "Failed to apply capability-based display modes!")) {
        new_state.m_modified.m_original_modes = original_display_modes;
        return true;
      }

      // Degrade: resolution-only (highest RR)
      if (config.m_resolution && !config.m_refresh_rate) {
        // Already resolution-only requested; nothing else to compute here.
      } else if (config.m_resolution) {
        DeviceDisplayModeMap cand_res_only = original_display_modes; bool any_ro {false};
        for (const auto &id : targets) {
          const auto disp_name_str = m_dd_api->getDisplayName(id);
          const std::wstring disp_name(disp_name_str.begin(), disp_name_str.end());
          const auto modes = enumerateSupportedModes(disp_name);
          const auto pick = pickBestMode(modes, config.m_resolution, std::nullopt, original_display_modes.at(id).m_resolution);
          if (pick) { cand_res_only[id].m_resolution = {pick->w, pick->h}; cand_res_only[id].m_refresh_rate = pick->rr; any_ro = true; }
        }
        if (any_ro && try_change(cand_res_only, "Changing display modes (resolution-only, capability-based) to:\n", "Failed to apply resolution-only display modes!")) {
          new_state.m_modified.m_original_modes = original_display_modes;
          DD_LOG(warning) << "Applied resolution-only fallback (capabilities).";
          return true;
        }
      }

      // Degrade: refresh-only (keep res)
      if (config.m_refresh_rate && !config.m_resolution) {
        DeviceDisplayModeMap cand_rr_only = original_display_modes; bool any_fps {false};
        for (const auto &id : targets) {
          const auto disp_name_str = m_dd_api->getDisplayName(id);
          const std::wstring disp_name(disp_name_str.begin(), disp_name_str.end());
          const auto modes = enumerateSupportedModes(disp_name);
          const auto pick = pickBestMode(modes, std::nullopt, config.m_refresh_rate, original_display_modes.at(id).m_resolution);
          if (pick) { cand_rr_only[id].m_resolution = {pick->w, pick->h}; cand_rr_only[id].m_refresh_rate = pick->rr; any_fps = true; }
        }
        if (any_fps && try_change(cand_rr_only, "Changing display modes (refresh-only, capability-based) to:\n", "Failed to apply refresh-only display modes!")) {
          new_state.m_modified.m_original_modes = original_display_modes;
          DD_LOG(warning) << "Applied refresh-only fallback (capabilities).";
          return true;
        }
      }

      DD_LOG(warning) << "Display mode change failed; proceeding without mode changes to preserve topology/device selection.";
      return true;
    }

    if (might_need_to_restore) {
      if (!try_change(cached_display_modes, "Changing display modes back to:\n", "Failed to restore original display modes!")) {
        // Error already logged
        return false;
      }
    }

    return true;
  }

  [[nodiscard]] bool SettingsManager::prepareHdrStates(const SingleDisplayConfiguration &config, const std::string &device_to_configure, const std::set<std::string> &additional_devices_to_configure, DdGuardFn &guard_fn, SingleDisplayConfigState &new_state, bool &system_settings_touched) {
    const auto &cached_state {m_persistence_state->getState()};
    const auto cached_hdr_states {cached_state ? cached_state->m_modified.m_original_hdr_states : HdrStateMap {}};
    const bool change_required {config.m_hdr_state};
    const bool might_need_to_restore {!cached_hdr_states.empty()};

    HdrStateMap current_hdr_states;
    if (change_required || might_need_to_restore) {
      current_hdr_states = m_dd_api->getCurrentHdrStates(win_utils::flattenTopology(new_state.m_modified.m_topology));
      if (current_hdr_states.empty()) {
        DD_LOG(error) << "Failed to get current HDR states!";
        return false;
      }
    }

    const auto try_change {[&](const HdrStateMap &new_states, const auto info_preamble, const auto error_log) {
      if (current_hdr_states != new_states) {
        DD_LOG(info) << info_preamble << toJson(new_states);
        if (!m_dd_api->setHdrStates(new_states)) {
          DD_LOG(error) << error_log;
          return false;
        }

        const auto after_states = m_dd_api->getCurrentHdrStates(win_utils::flattenTopology(new_state.m_modified.m_topology));
        if (after_states != current_hdr_states) {
          system_settings_touched = true;
          guard_fn = win_utils::hdrStateGuardFn(*m_dd_api, current_hdr_states);
        }
      }

      return true;
    }};

    if (change_required) {
      const bool configuring_primary_devices {config.m_device_id.empty()};
      const auto original_hdr_states {cached_hdr_states.empty() ? current_hdr_states : cached_hdr_states};
      const auto new_hdr_states {win_utils::computeNewHdrStates(config.m_hdr_state, configuring_primary_devices, device_to_configure, additional_devices_to_configure, original_hdr_states)};

      if (!try_change(new_hdr_states, "Changing HDR states to:\n", "Failed to apply new configuration, because new HDR states could not be set!")) {
        // Error already logged
        return false;
      }

      // Here we preserve the data from persistence (unless there's none) as in the end that is what we want to go back to.
      new_state.m_modified.m_original_hdr_states = original_hdr_states;
      return true;
    }

    if (might_need_to_restore) {
      if (!try_change(cached_hdr_states, "Changing HDR states back to:\n", "Failed to restore original HDR states!")) {
        // Error already logged
        return false;
      }
    }

    return true;
  }
}  // namespace display_device
