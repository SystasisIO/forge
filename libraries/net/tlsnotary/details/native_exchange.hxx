#pragma once

namespace forge::net::tlsnotary::detail {

class native_exchange : public std::enable_shared_from_this<native_exchange> {
 public:
   native_exchange(forge::asio::runtime& runtime, forge::asio::compute::executor compute, std::filesystem::path worker,
                   prover_request request, std::chrono::steady_clock::time_point deadline);
   native_exchange(forge::asio::runtime& runtime, forge::asio::compute::executor compute, std::filesystem::path worker,
                   verifier_request request, std::chrono::steady_clock::time_point deadline);
   ~native_exchange();
   void request_stop() noexcept;
   bool quiescent() const noexcept;
   std::chrono::steady_clock::time_point deadline() const noexcept;
   static boost::asio::awaitable<result> run(std::shared_ptr<native_exchange> self);
   static boost::asio::awaitable<void> discard(std::shared_ptr<native_exchange> self);

 private:
   void spawn();
   void prepare();
   boost::asio::awaitable<void> watch_worker();
   void decide_handoff(bool started) noexcept;
   void kill_owned() noexcept;
   void adopt();
   void close_ipc_on_strand() noexcept;
   void stop_on_strand() noexcept;
   boost::asio::awaitable<void> send(std::uint8_t kind, std::vector<std::uint8_t> bytes);
   boost::asio::awaitable<void> read_channel(bool server, std::uint32_t credit);
   boost::asio::awaitable<void> write_channel(bool server, std::vector<std::uint8_t> bytes, bool eof);
   void launch(boost::asio::awaitable<void> operation);
   std::vector<std::uint8_t> bootstrap() const;
   result decode(std::span<const std::uint8_t> payload) const;
   boost::asio::strand<boost::asio::io_context::executor_type> _strand;
   boost::asio::local::stream_protocol::socket _socket;
   boost::asio::steady_timer _timer;
   forge::asio::compute::executor _compute;
   std::filesystem::path _worker;
   forge::net::transport::stream _peer;
   forge::net::transport::stream _server;
   source_constraints _source;
   session_options _options;
   std::vector<std::uint8_t> _request;
   bool _prover = false;
   std::atomic<int> _launched_pid{-1};
   std::atomic<bool> _ownership_quiescent{true};
   int _pid = -1;
   int _native_status = 0;
   std::unique_ptr<boost::process::v2::process_handle> _process;
   bool _reap_pending = false;
   std::mutex _handoff_mutex;
   std::condition_variable _handoff_ready;
   bool _spawn_ready = false;
   bool _handoff_decided = false;
   bool _reaper_started = false;
   std::exception_ptr _setup_error;
   std::exception_ptr _worker_error;
   bool _worker_done = false;
   bool _watching_worker = false;
   std::optional<forge::asio::compute::operation<void>> _spawn_operation;
   std::atomic<int> _endpoint{-1};
   std::atomic<bool> _stopped{false};
   std::atomic<bool> _terminal{false};
   std::atomic<bool> _abort{false};
   bool _finishing = false;
   bool _expired = false;
   bool _timer_pending = false;
   std::exception_ptr _error;
   forge::asio::gate _writer;
   forge::asio::notification _changed;
   std::size_t _pumps = 0;
   bool _reading[2] = {false, false};
   bool _writing[2] = {false, false};
   bool _read_eof[2] = {false, false};
   bool _write_eof[2] = {false, false};
   std::vector<std::uint8_t> _pending[2];
   std::uint64_t _incoming[2] = {0, 0};
   std::uint64_t _outgoing[2] = {0, 0};
};
} // namespace forge::net::tlsnotary::detail
