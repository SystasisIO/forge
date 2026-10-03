#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace forge::net::p2p::detail {

class lifecycle_stop_listener;
class lifecycle_wakeup;
class lifecycle_stop_subscription;

class lifecycle_stop_source final : public std::enable_shared_from_this<lifecycle_stop_source> {
 private:
   struct observer {
      std::weak_ptr<lifecycle_stop_listener> listener;
   };

 public:
   [[nodiscard]] static std::shared_ptr<lifecycle_stop_source> create();

   [[nodiscard]] bool stop_requested() const noexcept;
   void request_stop() noexcept;

 private:
   lifecycle_stop_source() = default;

   [[nodiscard]] lifecycle_stop_subscription subscribe(std::weak_ptr<lifecycle_stop_listener> listener);
   void unsubscribe(const std::shared_ptr<observer>& value) noexcept;

   mutable std::mutex mutex_;
   std::atomic_bool stop_requested_ = false;
   std::vector<std::shared_ptr<observer>> observers_;

   friend class lifecycle_stop_subscription;
   friend class lifecycle_tracker;
};

class lifecycle_stop_subscription {
 public:
   lifecycle_stop_subscription() = default;
   lifecycle_stop_subscription(const lifecycle_stop_subscription&) = delete;
   lifecycle_stop_subscription& operator=(const lifecycle_stop_subscription&) = delete;
   lifecycle_stop_subscription(lifecycle_stop_subscription&& other) noexcept;
   lifecycle_stop_subscription& operator=(lifecycle_stop_subscription&& other) noexcept;
   ~lifecycle_stop_subscription();

   void reset() noexcept;

 private:
   lifecycle_stop_subscription(std::shared_ptr<lifecycle_stop_source> source,
                               std::shared_ptr<lifecycle_stop_source::observer> observer);

   std::shared_ptr<lifecycle_stop_source> source_;
   std::shared_ptr<lifecycle_stop_source::observer> observer_;

   friend class lifecycle_stop_source;
};

class lifecycle_tracker {
 private:
   struct state {
      struct operation_context {
         explicit operation_context(boost::asio::any_io_executor executor);

         boost::asio::strand<boost::asio::any_io_executor> strand;
      };

      explicit state(boost::asio::any_io_executor executor_value);
      void release(std::uint64_t id) noexcept;

      boost::asio::any_io_executor executor;
      mutable std::mutex mutex;
      lifecycle_phase phase = lifecycle_phase::idle;
      bool stop_requested = false;
      std::uint64_t next_operation_id = 1;
      std::map<std::uint64_t, std::shared_ptr<operation_context>> operations;
      std::shared_ptr<lifecycle_stop_source> stop_source;
      std::shared_ptr<lifecycle_wakeup> changed;
   };

 public:
   class operation {
    public:
      operation() = default;
      operation(const operation&) = delete;
      operation& operator=(const operation&) = delete;
      operation(operation&& other) noexcept;
      operation& operator=(operation&& other) noexcept;
      ~operation();

      [[nodiscard]] bool active() const noexcept;
      [[nodiscard]] boost::asio::any_io_executor executor() const noexcept;
      [[nodiscard]] std::shared_ptr<lifecycle_stop_source> stop_source() const noexcept;
      void release() noexcept;

    private:
      operation(std::shared_ptr<state> state, std::uint64_t id, std::shared_ptr<state::operation_context> context);

      std::shared_ptr<state> state_;
      std::uint64_t id_ = 0;
      std::shared_ptr<state::operation_context> context_;

      friend class lifecycle_tracker;
   };

   explicit lifecycle_tracker(boost::asio::any_io_executor executor);

   [[nodiscard]] bool begin_start() noexcept;
   void set_phase(lifecycle_phase value) noexcept;
   [[nodiscard]] lifecycle_phase phase() const noexcept;
   [[nodiscard]] bool stop_requested() const noexcept;
   [[nodiscard]] operation track() noexcept;
   [[nodiscard]] static lifecycle_stop_subscription
   subscribe_stop(const std::shared_ptr<lifecycle_stop_source>& source,
                  std::weak_ptr<lifecycle_stop_listener> listener);
   void request_stop() noexcept;
   boost::asio::awaitable<void> wait() const;
   void finish_stop() noexcept;

 private:
   std::shared_ptr<state> state_;
};

} // namespace forge::net::p2p::detail
