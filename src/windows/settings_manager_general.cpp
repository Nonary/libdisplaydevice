/**
 * @file src/windows/settings_manager_general.cpp
 * @brief Definitions for the leftover (general) methods in SettingsManager.
 */
// class header include
#include "display_device/windows/settings_manager.h"

// local includes
#include "display_device/logging.h"
#include "display_device/noop_audio_context.h"
#include "display_device/windows/json.h"
#include <boost/scope/scope_exit.hpp>
#include "display_device/windows/settings_utils.h"
#include "display_device/windows/win_display_device.h"

namespace display_device {
  SettingsManager::SettingsManager(
    std::shared_ptr<WinDisplayDeviceInterface> dd_api,
    std::shared_ptr<AudioContextInterface> audio_context_api,
    std::unique_ptr<PersistentState> persistent_state,
    WinWorkarounds workarounds
  ):
      m_dd_api {std::move(dd_api)},
      m_audio_context_api {std::move(audio_context_api)},
      m_persistence_state {std::move(persistent_state)},
      m_workarounds {std::move(workarounds)} {
    if (!m_dd_api) {
      throw std::logic_error {"Nullptr provided for WinDisplayDeviceInterface in SettingsManager!"};
    }

    if (!m_audio_context_api) {
      m_audio_context_api = std::make_shared<NoopAudioContext>();
    }

    if (!m_persistence_state) {
      throw std::logic_error {"Nullptr provided for PersistentState in SettingsManager!"};
    }

    DD_LOG(info) << "Provided workaround settings for SettingsManager:\n"
                 << toJson(m_workarounds);
  }

  EnumeratedDeviceList SettingsManager::enumAvailableDevices() const {
    return m_dd_api->enumAvailableDevices();
  }

  std::string SettingsManager::getDisplayName(const std::string &device_id) const {
    return m_dd_api->getDisplayName(device_id);
  }

  bool SettingsManager::resetPersistence() {
    DD_LOG(info) << "Trying to reset persistent display device settings.";
    if (const auto &cached_state {m_persistence_state->getState()}; !cached_state) {
      return true;
    }

    if (!m_persistence_state->persistState(std::nullopt)) {
      DD_LOG(error) << "Failed to clear persistence!";
      return false;
    }

    if (m_audio_context_api->isCaptured()) {
      m_audio_context_api->release();
    }
    return true;
  }

  std::optional<std::vector<std::uint8_t>> SettingsManager::exportRestoreProfile() const {
    if (!m_dd_api->isApiAccessAvailable()) {
      DD_LOG(error) << "Export profile: API temporarily unavailable.";
      return std::nullopt;
    }

    const auto topology {m_dd_api->getCurrentTopology()};
    if (!m_dd_api->isTopologyValid(topology)) {
      DD_LOG(error) << "Export profile: current topology is invalid:\n" << toJson(topology);
      return std::nullopt;
    }

    const auto device_ids {win_utils::flattenTopology(topology)};
    const auto modes {m_dd_api->getCurrentDisplayModes(device_ids)};
    if (modes.empty()) {
      DD_LOG(error) << "Export profile: failed to get current display modes!";
      return std::nullopt;
    }

    const auto hdr_states {m_dd_api->getCurrentHdrStates(device_ids)};
    if (hdr_states.empty()) {
      DD_LOG(error) << "Export profile: failed to get current HDR states!";
      return std::nullopt;
    }

    // Collect set of primary devices present in topology.
    std::set<std::string> primary_devices;
    for (const auto &id : device_ids) {
      if (m_dd_api->isPrimary(id)) {
        primary_devices.insert(id);
      }
    }

    const auto original_primary_device {win_utils::getPrimaryDevice(*m_dd_api, topology)};

    SingleDisplayConfigState snapshot {
      {topology, primary_devices},
      {topology, modes, hdr_states, original_primary_device}
    };

    const auto json {toJson(snapshot)};
    return std::vector<std::uint8_t> {std::begin(json), std::end(json)};
  }

  SettingsManager::RevertResult SettingsManager::restoreFromProfile(const std::vector<std::uint8_t> &data) {
    if (!m_dd_api->isApiAccessAvailable()) {
      return RevertResult::ApiTemporarilyUnavailable;
    }

    const auto current_topology {m_dd_api->getCurrentTopology()};
    if (!m_dd_api->isTopologyValid(current_topology)) {
      DD_LOG(error) << "Restore profile: current topology is invalid:\n" << toJson(current_topology);
      return RevertResult::TopologyIsInvalid;
    }

    // Parse snapshot
    const std::string json_str {data.begin(), data.end()};
    SingleDisplayConfigState snapshot;
    std::string parse_error;
    if (!fromJson(json_str, snapshot, &parse_error)) {
      DD_LOG(error) << "Restore profile: failed to parse profile JSON: " << parse_error;
      return RevertResult::PersistenceSaveFailed;  // best mapping for malformed input
    }

    bool system_settings_touched {false};
    boost::scope::scope_exit hdr_blank_always_executed_guard {[this, &system_settings_touched]() {
      if (system_settings_touched) {
        win_utils::blankHdrStates(*m_dd_api, m_workarounds.m_hdr_blank_delay);
      }
    }};

    // 1) Switch to modified topology (the one associated with per-device modes/HDR).
    if (!m_dd_api->isTopologyValid(snapshot.m_modified.m_topology)) {
      DD_LOG(error) << "Restore profile: modified topology invalid:\n" << toJson(snapshot.m_modified.m_topology);
      return RevertResult::TopologyIsInvalid;
    }

    if (!m_dd_api->isTopologyTheSame(current_topology, snapshot.m_modified.m_topology)) {
      system_settings_touched = true;
      if (!m_dd_api->setTopology(snapshot.m_modified.m_topology)) {
        DD_LOG(error) << "Restore profile: failed to set modified topology!";
        return RevertResult::SwitchingTopologyFailed;
      }
    }

    // 2) Restore HDR states if provided.
    if (!snapshot.m_modified.m_original_hdr_states.empty()) {
      const auto current_states {m_dd_api->getCurrentHdrStates(win_utils::flattenTopology(snapshot.m_modified.m_topology))};
      if (current_states != snapshot.m_modified.m_original_hdr_states) {
        system_settings_touched = true;
        DD_LOG(info) << "Restore profile: applying HDR states:\n" << toJson(snapshot.m_modified.m_original_hdr_states);
        if (!m_dd_api->setHdrStates(snapshot.m_modified.m_original_hdr_states)) {
          return RevertResult::RevertingHdrStatesFailed;
        }
      }
    }

    // 3) Restore display modes if provided (strict restore).
    if (!snapshot.m_modified.m_original_modes.empty()) {
      const auto current_modes {m_dd_api->getCurrentDisplayModes(win_utils::flattenTopology(snapshot.m_modified.m_topology))};
      if (current_modes != snapshot.m_modified.m_original_modes) {
        system_settings_touched = true;
        WinDisplayDevice::setForceStrictModes(true);
        boost::scope::scope_exit strict_reset {[]() { WinDisplayDevice::setForceStrictModes(false); }};
        DD_LOG(info) << "Restore profile: applying display modes (strict):\n" << toJson(snapshot.m_modified.m_original_modes);
        if (!m_dd_api->setDisplayModes(snapshot.m_modified.m_original_modes)) {
          return RevertResult::RevertingDisplayModesFailed;
        }
      }
    }

    // 4) Restore primary device if provided.
    if (!snapshot.m_modified.m_original_primary_device.empty()) {
      const auto current_primary {win_utils::getPrimaryDevice(*m_dd_api, snapshot.m_modified.m_topology)};
      if (current_primary != snapshot.m_modified.m_original_primary_device) {
        system_settings_touched = true;
        DD_LOG(info) << "Restore profile: setting primary device to: " << toJson(snapshot.m_modified.m_original_primary_device);
        if (!m_dd_api->setAsPrimary(snapshot.m_modified.m_original_primary_device)) {
          return RevertResult::RevertingPrimaryDeviceFailed;
        }
      }
    }

    // 5) Switch to initial topology.
    if (!m_dd_api->isTopologyValid(snapshot.m_initial.m_topology)) {
      DD_LOG(error) << "Restore profile: initial topology invalid:\n" << toJson(snapshot.m_initial.m_topology);
      return RevertResult::TopologyIsInvalid;
    }

    if (!m_dd_api->isTopologyTheSame(snapshot.m_modified.m_topology, snapshot.m_initial.m_topology)) {
      system_settings_touched = true;
      if (!m_dd_api->setTopology(snapshot.m_initial.m_topology)) {
        DD_LOG(error) << "Restore profile: failed to set initial topology!";
        return RevertResult::SwitchingTopologyFailed;
      }
    }

    return RevertResult::Ok;
  }
}  // namespace display_device
