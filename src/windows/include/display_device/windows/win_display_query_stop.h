#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <utility>

namespace display_device {
  // Distinguish cancellation from an unavailable display at every layer of the
  // lookup. A capture worker handles this after its normal cleanup has run.
  struct DisplayQueryStopped {};

  namespace detail {
    inline thread_local std::stop_token display_query_stop;
  }

  // Opt in only around a capture worker's display selection/open attempts.
  // The recovery policy and other threads' display operations are independent.
  class DisplayQueryStopGuard {
   public:
    explicit DisplayQueryStopGuard(std::stop_token stop) noexcept:
        m_previous(std::exchange(detail::display_query_stop, std::move(stop))) {}
    DisplayQueryStopGuard(const DisplayQueryStopGuard &) = delete;
    DisplayQueryStopGuard &operator=(const DisplayQueryStopGuard &) = delete;
    ~DisplayQueryStopGuard() {
      detail::display_query_stop = std::move(m_previous);
    }

   private:
    std::stop_token m_previous;
  };

  inline void check_display_query_stop() {
    if (detail::display_query_stop.stop_requested()) {
      throw DisplayQueryStopped {};
    }
  }

  template<class Query>
  auto run_display_query(Query &&query) {
    check_display_query_stop();
    auto result = std::forward<Query>(query)();
    check_display_query_stop();
    return result;
  }

  template<class Sleep>
  void wait_for_display_query_retry(std::chrono::milliseconds delay, Sleep sleep) {
    const auto &stop = detail::display_query_stop;
    if (!stop.stop_possible()) {
      sleep(delay);
      return;
    }

    std::mutex mutex;
    std::unique_lock lock {mutex};
    std::condition_variable_any wake;
    wake.wait_for(lock, stop, delay, [] { return false; });
    check_display_query_stop();
  }
}  // namespace display_device
