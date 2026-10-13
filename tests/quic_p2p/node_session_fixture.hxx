#pragma once

namespace forge::net::p2p {

// Compiled only in the isolated pubsub terminal target, never beside another friend definition.
struct node_session_fixture {
   enum class retirement { peer, single, topology, admission };
   enum class terminal_result {
      success, transport_closed, transport_canceled, yamux_closed, yamux_canceled,
      yamux_protocol, transport_protocol, io, allocation,
   };
   static node::options options(std::string_view name);
   static peer_id peer(std::uint8_t value);
   static void seed(node& owner, const peer_id& peer, std::uint64_t id);
   static void outbound(node& owner, const peer_id& peer, std::uint64_t id);
   static void admit(forge::asio::runtime& runtime, node& owner, const peer_id& peer, std::uint64_t id);
   static stream input(const pubsub::message& value);
   static void positive_retention(retirement kind);
   static void whole_batch(bool admission);
   static void surviving_session(bool replacement);
   static void frozen_negative();
   static void preclaim_and_pressure();
   static void refusals();
   static void handler_copy_failure();
   static void claim_allocation_rollback();
   static void invalid_signature();
   static void terminal_close(terminal_result result);
   static void native_retired_validation(bool retry);
   static void native_reflected_self_origin();
   static void native_received_policy(bool sign);

 private:
   [[noreturn]] static void fail_terminal_join() noexcept;
   class terminal_transport;
   class message_input;
   struct throwing_handler_copy;
};

class node_session_fixture::terminal_transport final : public forge::net::transport::detail::session_concept {
 public:
   terminal_transport();
   terminal_transport(terminal_result result, std::shared_ptr<std::promise<void>> entered,
                      std::shared_ptr<forge::asio::notification> barrier,
                      std::shared_ptr<std::atomic_bool> barrier_passed,
                      std::shared_ptr<detail::session_teardown::ticket> native_ticket,
                      resource_manager::memory_reservation memory,
                      resource_manager::file_descriptor_reservation descriptor);
   ~terminal_transport() override;
   bool valid() const noexcept override;
   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override;
   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override;
   boost::asio::awaitable<void> async_close() override;
   void cancel() override;

 private:
   std::atomic_bool _open{true};
   terminal_result _result = terminal_result::success;
   std::shared_ptr<std::promise<void>> _entered;
   std::shared_ptr<forge::asio::notification> _barrier;
   forge::asio::notification::epoch_type _barrier_epoch = 0;
   std::shared_ptr<std::atomic_bool> _barrier_passed;
   std::shared_ptr<detail::session_teardown::ticket> _native_ticket;
   resource_manager::memory_reservation _memory;
   resource_manager::file_descriptor_reservation _descriptor;
};

class node_session_fixture::message_input final : public forge::net::transport::detail::stream_concept {
 public:
   explicit message_input(std::vector<std::uint8_t> bytes);
   bool valid() const noexcept override;
   std::int64_t id() const noexcept override;
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t>) override;
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override;
   boost::asio::awaitable<void> async_close() override;
   void cancel() override;

 private:
   std::vector<std::uint8_t> _bytes;
};

struct node_session_fixture::throwing_handler_copy {
   std::shared_ptr<std::atomic_bool> armed;
   std::function<void()> observe;
   throwing_handler_copy(std::shared_ptr<std::atomic_bool> value, std::function<void()> callback);
   throwing_handler_copy(const throwing_handler_copy& other);
   boost::asio::awaitable<pubsub::validation_result> operator()(pubsub::event) const;
};

} // namespace forge::net::p2p
