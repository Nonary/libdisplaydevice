#pragma once

#include <chrono>
#include <functional>

namespace display_device {
  enum class DisplayRecoveryBehavior {
    Automatic,
    Skip,
  };

  struct DisplayQueryStopped {};

  struct DisplayQueryCancellation {
    std::function<bool()> stopped;
    std::function<bool(std::chrono::milliseconds)> wait_for_stop;
  };

  class DisplayRecoveryBehaviorGuard {
   public:
    explicit DisplayRecoveryBehaviorGuard(
      DisplayRecoveryBehavior behavior,
      DisplayQueryCancellation cancellation = {}
    );
    DisplayRecoveryBehaviorGuard(const DisplayRecoveryBehaviorGuard &) = delete;
    DisplayRecoveryBehaviorGuard &operator=(const DisplayRecoveryBehaviorGuard &) = delete;
    ~DisplayRecoveryBehaviorGuard();

   private:
    DisplayQueryCancellation m_cancellation;
    DisplayRecoveryBehavior m_previous_behavior;
    const DisplayQueryCancellation *m_previous_cancellation;
  };

  namespace detail {
    DisplayRecoveryBehavior current_display_recovery_behavior();
  }  // namespace detail

  void check_display_query_stop();
  void wait_for_display_query_retry(
    std::chrono::milliseconds delay,
    const std::function<void(std::chrono::milliseconds)> &sleep = {}
  );
}  // namespace display_device
