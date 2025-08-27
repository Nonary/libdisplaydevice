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
// utils used: flattenTopology, getPrimaryDevice
#include "display_device/windows/settings_utils.h"

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

  std::optional<DisplaySettingsSnapshot> SettingsManager::exportCurrentSettings() const {
    const auto api_access {m_dd_api->isApiAccessAvailable()};
    DD_LOG(info) << "Exporting current display device settings. API is available: " << toJson(api_access);
    if (!api_access) {
      return std::nullopt;
    }

    const auto topology {m_dd_api->getCurrentTopology()};
    if (!m_dd_api->isTopologyValid(topology)) {
      DD_LOG(error) << "Retrieved current topology is invalid:\n" << toJson(topology);
      return std::nullopt;
    }

    const auto devices_flat {win_utils::flattenTopology(topology)};
    const auto modes {m_dd_api->getCurrentDisplayModes(devices_flat)};
    if (modes.empty()) {
      DD_LOG(error) << "Failed to get current display modes during export!";
      return std::nullopt;
    }

    const auto hdr_states {m_dd_api->getCurrentHdrStates(devices_flat)};
    const auto primary_device {win_utils::getPrimaryDevice(*m_dd_api, topology)};

    DisplaySettingsSnapshot snapshot {topology, modes, hdr_states, primary_device};
    DD_LOG(info) << "Exported snapshot:\n" << toJson(snapshot);
    return snapshot;
  }
}  // namespace display_device
