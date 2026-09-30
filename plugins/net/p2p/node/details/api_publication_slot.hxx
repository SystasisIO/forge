#pragma once

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <mutex>

namespace forge::plugins::net::p2p::node::detail {

class api_publication_generation;

class api_publication_slot final : public std::enable_shared_from_this<api_publication_slot> {
 public:
   explicit api_publication_slot(forge::net::p2p::protocol_id protocol);
   ~api_publication_slot();

   api_publication_slot(const api_publication_slot&) = delete;
   api_publication_slot& operator=(const api_publication_slot&) = delete;

   [[nodiscard]] const forge::net::p2p::protocol_id& protocol() const noexcept;
   [[nodiscard]] forge::net::p2p::node::protocol_handler handler();
   [[nodiscard]] std::shared_ptr<api_publication_generation>
   replace(std::shared_ptr<api_publication_generation> generation) noexcept;
   [[nodiscard]] std::shared_ptr<api_publication_generation>
   clear_if(const std::shared_ptr<api_publication_generation>& generation) noexcept;
   [[nodiscard]] std::shared_ptr<api_publication_generation> clear() noexcept;
   void attach(const std::shared_ptr<forge::net::p2p::node>& node);
   void detach() noexcept;

 private:
   boost::asio::awaitable<void> accept(forge::net::p2p::node::incoming_protocol_stream stream);

   forge::net::p2p::protocol_id protocol_;
   std::shared_ptr<api_publication_generation> current_;
   std::mutex mutex_;
   std::shared_ptr<forge::net::p2p::node> node_;
   bool registered_ = false;
};

[[nodiscard]] std::shared_ptr<api_publication_slot>
make_api_publication_slot(forge::net::p2p::protocol_id protocol);
[[nodiscard]] const forge::net::p2p::protocol_id&
api_publication_slot_protocol(const std::shared_ptr<api_publication_slot>& slot) noexcept;
[[nodiscard]] std::shared_ptr<api_publication_generation>
replace_api_publication_slot(const std::shared_ptr<api_publication_slot>& slot,
                             std::shared_ptr<api_publication_generation> generation) noexcept;
[[nodiscard]] std::shared_ptr<api_publication_generation>
clear_api_publication_slot_if(const std::shared_ptr<api_publication_slot>& slot,
                              const std::shared_ptr<api_publication_generation>& generation) noexcept;
[[nodiscard]] std::shared_ptr<api_publication_generation>
clear_api_publication_slot(const std::shared_ptr<api_publication_slot>& slot) noexcept;
void attach_api_publication_slot(const std::shared_ptr<api_publication_slot>& slot,
                                 const std::shared_ptr<forge::net::p2p::node>& node);
void detach_api_publication_slot(const std::shared_ptr<api_publication_slot>& slot) noexcept;

} // namespace forge::plugins::net::p2p::node::detail
