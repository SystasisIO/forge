#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <exception>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace forge::plugins::net::p2p::resolver::detail {

class publication_catalog final : public std::enable_shared_from_this<publication_catalog> {
 public:
   explicit publication_catalog(forge::asio::task::scheduler& scheduler);
   ~publication_catalog();

   publication_catalog(const publication_catalog&) = delete;
   publication_catalog& operator=(const publication_catalog&) = delete;

   [[nodiscard]] forge::api::p2p::publication
   publish(forge::plugins::net::p2p::node::api& p2p, forge::api::core::binding_plan plan,
           forge::net::p2p::protocol_id protocol, forge::api::transport::options options,
           std::vector<entry> entries, std::size_t max_apis);
   [[nodiscard]] std::vector<entry> snapshot() const;
   void request_close() noexcept;
   boost::asio::awaitable<void> async_close();

 private:
   class generation final {
    public:
      generation(std::string protocol, std::vector<entry> entries);

      generation(const generation&) = delete;
      generation& operator=(const generation&) = delete;

      void set_child(std::shared_ptr<forge::api::p2p::publication> child) noexcept;
      [[nodiscard]] const std::string& protocol() const noexcept;
      [[nodiscard]] const std::vector<entry>& entries() const noexcept;
      [[nodiscard]] bool active() const noexcept;
      [[nodiscard]] bool seal(std::shared_ptr<forge::api::p2p::publication>& child) noexcept;
      void close() noexcept;
      boost::asio::awaitable<void> async_close();

    private:
      std::string protocol_;
      std::vector<entry> entries_;
      mutable std::mutex mutex_;
      std::shared_ptr<forge::api::p2p::publication> child_;
      bool active_ = true;
   };

   struct retirement_record {
      const generation* identity = nullptr;
      std::shared_ptr<generation> value;
      forge::asio::task::handle handle;
   };

   void close_generation(const std::shared_ptr<generation>& generation) noexcept;
   void schedule_retirement(const std::shared_ptr<generation>& generation) noexcept;
   void finish_retirement(const generation* generation, std::exception_ptr failure) noexcept;
   void abandon_retirement(const generation* generation) noexcept;
   void reject_retirement(const generation* generation, std::exception_ptr failure) noexcept;
   void remember_retirement_failure_locked(std::exception_ptr failure) noexcept;
   void validate_publish_locked(const std::string& protocol, const std::vector<entry>& entries,
                                std::size_t max_apis) const;
   void validate_retirement_backlog_locked(std::size_t max_apis) const;
   [[nodiscard]] bool cancel_reservation(const std::string& protocol,
                                         const std::shared_ptr<generation>& generation,
                                         bool remove_order) noexcept;
   [[nodiscard]] bool finish_publish_locked() noexcept;
   boost::asio::awaitable<void> wait_for_publishes();
   boost::asio::awaitable<void> wait_for_retirements();

   mutable std::mutex mutex_;
   forge::asio::task::scheduler* scheduler_ = nullptr;
   boost::asio::any_io_executor owner_executor_;
   std::map<std::string, std::shared_ptr<generation>, std::less<>> active_;
   std::map<std::string, std::shared_ptr<generation>, std::less<>> pending_;
   std::multimap<std::string, std::shared_ptr<generation>, std::less<>> closing_;
   std::vector<std::string> order_;
   forge::asio::notification publishes_ready_;
   forge::asio::notification retirements_ready_;
   std::list<retirement_record> retirements_;
   std::exception_ptr retirement_failure_;
   std::size_t inflight_publishes_ = 0;
   std::size_t inflight_retirements_ = 0;
   bool closed_ = false;
};

} // namespace forge::plugins::net::p2p::resolver::detail
