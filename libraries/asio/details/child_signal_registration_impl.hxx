#pragma once

#include <atomic>
#include <csignal>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/signal_set.hpp>

namespace forge::asio {

struct child_signal_registration::impl {
   void acquire(boost::asio::any_io_executor executor, void (*after_install)());
   void release();
   [[nodiscard]] bool registered() const noexcept;
#if defined(__APPLE__)
   static bool same_disposition(const struct sigaction&) noexcept;
   static bool cold_default(const struct sigaction&) noexcept;
   static std::mutex mutex;
   static std::size_t registrations;
   static std::optional<struct sigaction> disposition;
#endif
   std::unique_ptr<boost::asio::signal_set> owner;
   std::atomic_bool active = false;
};

} // namespace forge::asio
