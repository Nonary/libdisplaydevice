#include "display_device/windows/win_api_recovery.h"

#include <thread>
#include <utility>

namespace display_device {
  namespace {
    struct DisplayRecoveryContext {
      DisplayRecoveryBehavior behavior = DisplayRecoveryBehavior::Automatic;
      const DisplayQueryCancellation *cancellation = nullptr;
    };

    // Keep one constant-initialized context in the library, including DLL builds.
    constinit thread_local DisplayRecoveryContext display_recovery;
  }  // namespace

  DisplayRecoveryBehavior detail::current_display_recovery_behavior() {
    return display_recovery.behavior;
  }

  DisplayRecoveryBehaviorGuard::DisplayRecoveryBehaviorGuard(
    DisplayRecoveryBehavior behavior,
    DisplayQueryCancellation cancellation
  ):
      m_cancellation(std::move(cancellation)),
      m_previous_behavior(display_recovery.behavior),
      m_previous_cancellation(display_recovery.cancellation) {
    display_recovery.behavior = behavior;
    if (m_cancellation.stopped) {
      display_recovery.cancellation = &m_cancellation;
    }
  }

  DisplayRecoveryBehaviorGuard::~DisplayRecoveryBehaviorGuard() {
    display_recovery = {m_previous_behavior, m_previous_cancellation};
  }

  void check_display_query_stop() {
    const auto *cancellation = display_recovery.cancellation;
    if (cancellation && cancellation->stopped()) {
      throw DisplayQueryStopped {};
    }
  }

  void wait_for_display_query_retry(
    std::chrono::milliseconds delay,
    const std::function<void(std::chrono::milliseconds)> &sleep
  ) {
    check_display_query_stop();
    const auto *cancellation = display_recovery.cancellation;
    if (cancellation && cancellation->wait_for_stop) {
      if (cancellation->wait_for_stop(delay)) {
        throw DisplayQueryStopped {};
      }
    } else if (sleep) {
      sleep(delay);
    } else {
      std::this_thread::sleep_for(delay);
    }
    check_display_query_stop();
  }
}  // namespace display_device
