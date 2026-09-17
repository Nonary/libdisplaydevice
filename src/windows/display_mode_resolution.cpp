/**
 * @file src/windows/display_mode_resolution.cpp
 * @brief Definitions for Windows display mode merge/resolution helpers.
 */
// class header include
#include "display_device/windows/display_mode_resolution.h"

// system includes
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace display_device::win_utils {
  namespace {

    struct DisplayModeKey {
      unsigned int m_width;
      unsigned int m_height;
      unsigned int m_refresh_num;
      unsigned int m_refresh_den;

      bool operator==(const DisplayModeKey &o) const {
        return m_width == o.m_width && m_height == o.m_height && m_refresh_num == o.m_refresh_num && m_refresh_den == o.m_refresh_den;
      }
    };

    struct DisplayModeKeyHash {
      std::size_t operator()(const DisplayModeKey &k) const noexcept {
        std::size_t hash = static_cast<std::size_t>(k.m_width);
        hash = (hash << 16) ^ static_cast<std::size_t>(k.m_height);
        hash = (hash << 16) ^ static_cast<std::size_t>(k.m_refresh_num);
        hash = (hash << 16) ^ static_cast<std::size_t>(k.m_refresh_den);
        return hash;
      }
    };

    DisplayModeKey makeKey(const DisplayMode &mode) {
      const unsigned int denominator = mode.m_refresh_rate.m_denominator == 0 ? 1U : mode.m_refresh_rate.m_denominator;
      return DisplayModeKey {mode.m_resolution.m_width, mode.m_resolution.m_height, mode.m_refresh_rate.m_numerator, denominator};
    }

    bool sameAspect(const Resolution &a, const Resolution &b) {
      const long long lhs = static_cast<long long>(a.m_width) * static_cast<long long>(b.m_height);
      const long long rhs = static_cast<long long>(b.m_width) * static_cast<long long>(a.m_height);
      return lhs == rhs;
    }

    double refreshToDouble(const Rational &r) {
      if (r.m_denominator == 0) {
        return 0.0;
      }
      return static_cast<double>(r.m_numerator) / static_cast<double>(r.m_denominator);
    }

    bool refreshRatesEqual(const Rational &lhs, const Rational &rhs) {
      if (lhs.m_denominator <= 0 || rhs.m_denominator <= 0) {
        return false;
      }
      const auto lhs_scaled = static_cast<long long>(lhs.m_numerator) * static_cast<long long>(rhs.m_denominator);
      const auto rhs_scaled = static_cast<long long>(rhs.m_numerator) * static_cast<long long>(lhs.m_denominator);
      return lhs_scaled == rhs_scaled;
    }

    std::optional<DisplayMode> pickClosestMode(const DisplayMode &requested_mode, const std::vector<DisplayMode> &candidates, bool require_same_aspect) {
      if (candidates.empty()) {
        return std::nullopt;
      }

      const auto req_area = static_cast<long long>(requested_mode.m_resolution.m_width) * static_cast<long long>(requested_mode.m_resolution.m_height);
      const double req_hz = refreshToDouble(requested_mode.m_refresh_rate);

      std::optional<DisplayMode> best;
      auto best_area_delta = std::numeric_limits<long long>::max();
      double best_refresh_delta = std::numeric_limits<double>::infinity();

      for (const auto &candidate : candidates) {
        if (require_same_aspect && !sameAspect(candidate.m_resolution, requested_mode.m_resolution)) {
          continue;
        }

        const auto area = static_cast<long long>(candidate.m_resolution.m_width) * static_cast<long long>(candidate.m_resolution.m_height);
        const auto area_delta = std::llabs(area - req_area);
        const double cand_hz = refreshToDouble(candidate.m_refresh_rate);
        const double hz_delta = std::abs(cand_hz - req_hz);

        if (!best || area_delta < best_area_delta || (area_delta == best_area_delta && hz_delta < best_refresh_delta)) {
          best = candidate;
          best_area_delta = area_delta;
          best_refresh_delta = hz_delta;
        }
      }

      return best;
    }

    std::optional<DisplayMode> pickPreferredResolutionMode(const DisplayMode &requested_mode, const std::vector<DisplayMode> &candidates, const std::optional<Resolution> &preferred_resolution) {
      if (!preferred_resolution) {
        return std::nullopt;
      }

      if (!sameAspect(*preferred_resolution, requested_mode.m_resolution)) {
        return std::nullopt;
      }

      std::optional<DisplayMode> best;
      double best_refresh_delta = std::numeric_limits<double>::infinity();
      const double req_hz = refreshToDouble(requested_mode.m_refresh_rate);

      for (const auto &candidate : candidates) {
        if (candidate.m_resolution.m_width != preferred_resolution->m_width || candidate.m_resolution.m_height != preferred_resolution->m_height) {
          continue;
        }

        const double cand_hz = refreshToDouble(candidate.m_refresh_rate);
        const double hz_delta = std::abs(cand_hz - req_hz);
        if (!best || hz_delta < best_refresh_delta) {
          best = candidate;
          best_refresh_delta = hz_delta;
        }
      }

      if (best) {
        return best;
      }

      return DisplayMode {*preferred_resolution, requested_mode.m_refresh_rate};
    }
  }  // namespace

  std::vector<DisplayMode> mergeDisplayModes(const std::vector<DisplayMode> &primary, const std::vector<DisplayMode> &secondary) {
    std::vector<DisplayMode> result;
    result.reserve(primary.size() + secondary.size());

    std::unordered_set<DisplayModeKey, DisplayModeKeyHash> seen;
    const auto append_unique = [&](const std::vector<DisplayMode> &modes) {
      for (const auto &mode : modes) {
        if (mode.m_resolution.m_width == 0 || mode.m_resolution.m_height == 0) {
          continue;
        }
        if (seen.insert(makeKey(mode)).second) {
          result.push_back(mode);
        }
      }
    };

    append_unique(primary);
    append_unique(secondary);
    return result;
  }

  bool supportedModesContainResolution(const std::vector<DisplayMode> &modes, const Resolution &resolution) {
    for (const auto &mode : modes) {
      if (mode.m_resolution.m_width == resolution.m_width && mode.m_resolution.m_height == resolution.m_height) {
        return true;
      }
    }
    return false;
  }

  DisplayMode resolveRequestedDisplayMode(const DisplayMode &requested_mode, const std::vector<DisplayMode> &supported_modes, const std::optional<Resolution> &preferred_resolution) {
    for (const auto &candidate : supported_modes) {
      if (candidate.m_resolution.m_width == requested_mode.m_resolution.m_width &&
          candidate.m_resolution.m_height == requested_mode.m_resolution.m_height &&
          refreshRatesEqual(candidate.m_refresh_rate, requested_mode.m_refresh_rate)) {
        return candidate;
      }
    }

    if (auto chosen = pickClosestMode(requested_mode, supported_modes, true)) {
      return *chosen;
    }

    if (auto chosen = pickPreferredResolutionMode(requested_mode, supported_modes, preferred_resolution)) {
      return *chosen;
    }

    // Keep the requested mode rather than switching to a different aspect ratio.
    // The previous "closest pixel area" fallback would select 5120x1440 for a
    // 3840x1680 Sunshine virtual display whenever DXGI omitted the custom mode.
    return requested_mode;
  }
}  // namespace display_device::win_utils
