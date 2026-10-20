#pragma once

namespace forge::swarm::runtime::detail {

class peer_session : public std::enable_shared_from_this<peer_session> {
 public:
   using client_call = forge::api::core::bidirectional_stream_call<protocol::peer_message, protocol::peer_message>;
   struct owned_request {
      core::transfer::request request;
      resource_budget::ticket capacity;
      std::shared_ptr<operation_state::piece_work> piece;
      std::uint64_t generation = 0;
   };
   struct command {
      protocol::peer_message message;
      resource_budget::ticket capacity;
   };
   peer_session(std::shared_ptr<operation_state>, std::uint64_t);
   static boost::asio::awaitable<void> metadata(std::shared_ptr<operation_state>,
      forge::api::auth::authenticated_caller, forge::api::core::stream_writer<protocol::metadata_chunk>);
   static boost::asio::awaitable<void> serve(std::shared_ptr<operation_state>,
      forge::api::auth::authenticated_caller,
      forge::api::core::duplex_stream<protocol::peer_message, protocol::peer_message>);
   static boost::asio::awaitable<void> connect(std::shared_ptr<operation_state>, std::string);
 private:
   static boost::asio::awaitable<void> authorize(std::shared_ptr<call_scope>, forge::api::auth::authenticated_caller, access_kind);
   static boost::asio::awaitable<void> serve_read(std::shared_ptr<peer_session>);
   static boost::asio::awaitable<void> serve_write(std::shared_ptr<peer_session>);
   static boost::asio::awaitable<void> watch_availability(std::shared_ptr<peer_session>);
   static boost::asio::awaitable<void> download_read(std::shared_ptr<peer_session>);
   static boost::asio::awaitable<void> connect_owned(std::shared_ptr<peer_session>, std::string);
   static boost::asio::awaitable<void> download_write(std::shared_ptr<peer_session>);
   static boost::asio::awaitable<void> receive(std::shared_ptr<peer_session>, protocol::block);
   static boost::asio::awaitable<void> send(std::shared_ptr<peer_session>, protocol::peer_message, std::uint64_t,
      std::optional<quota::reservation> = std::nullopt);
   static boost::asio::awaitable<void> paired(std::shared_ptr<peer_session>, bool serving);
   void retract(std::uint64_t);
   void interrupt() noexcept;
   std::shared_ptr<operation_state> _owner;
   std::uint64_t _number;
   std::shared_ptr<call_scope> _scope;
   std::shared_ptr<capabilities::connection> _connection;
   std::shared_ptr<client_call> _call;
   std::optional<forge::api::core::duplex_stream<protocol::peer_message, protocol::peer_message>> _server;
   std::mutex _mutex;
   std::map<std::uint64_t, owned_request> _requests;
   // Bounded commands contain no payload. Writer obtains byte capacity before I/O.
   std::deque<command> _commands;
   std::set<std::uint64_t> _canceled;
   std::set<std::uint64_t> _serving;
   std::optional<core::peer_id> _remote;
   std::vector<std::uint8_t> _inventory;
   bool _hello = false;
   bool _interested = false;
   bool _choked = true;
   std::uint32_t _integrity_failures = 0;
   forge::asio::notification _changed;
};

} // namespace forge::swarm::runtime::detail
