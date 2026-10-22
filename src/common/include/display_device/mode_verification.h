#pragma once

#include "display_device/types.h"

#include <cstdint>

namespace display_device {
  // Compare frequencies, not their representation. Do not accept invalid rates
  // or apply the tolerance used when selecting a supported streaming mode.
  // This comparison does not establish equality of display-path state, such as
  // Windows Dynamic Refresh Rate (DRR), which is not encoded by a rational alone.
  inline bool equalRefreshRates(const Rational &lhs, const Rational &rhs) {
    return lhs.m_numerator != 0 && rhs.m_numerator != 0 &&
           lhs.m_denominator != 0 && rhs.m_denominator != 0 &&
           static_cast<std::uint64_t>(lhs.m_numerator) * rhs.m_denominator ==
             static_cast<std::uint64_t>(rhs.m_numerator) * lhs.m_denominator;
  }

  template <typename ModeMap>
  bool equalDisplayModes(const ModeMap &lhs, const ModeMap &rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (const auto &[id, mode] : lhs) {
      const auto other = rhs.find(id);
      if (other == rhs.end() ||
          mode.m_resolution.m_width != other->second.m_resolution.m_width ||
          mode.m_resolution.m_height != other->second.m_resolution.m_height ||
          !equalRefreshRates(mode.m_refresh_rate, other->second.m_refresh_rate)) {
        return false;
      }
    }
    return true;
  }
}  // namespace display_device
