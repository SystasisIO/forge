#pragma once

// Included after the owning session module. One fresh helper owns one private
// inherited socket endpoint and one unmanaged Boost.Process async reap.
struct forge::vm::wasm::optimizing::compiler_session::impl::child : std::enable_shared_from_this<child> {
   // Diagnostic progress only: never used for admission, cancellation or reap.
   enum class deadline_phase {
      before_io,
      request_write,
      hello_header,
      hello_body,
      ack_write,
      reply_header,
      reply_body,
      output_eof,
      await_reap,
   };
   enum class socket_observation {
      not_observed,
      eof,
      byte,
      would_block,
      interrupted,
      error,
   };

   child(boost::asio::any_io_executor, const session_options&, compiler_protocol::exchange_id, crypto::digest::sha256,
         target_profile, bool preflight = false);
   ~child();
   void spawn(std::stop_token); // existing compute boundary; records PID before return
   static boost::asio::awaitable<std::vector<std::uint8_t>> run_owned(std::shared_ptr<child>, std::vector<std::uint8_t>,
                                                                      std::stop_token);
   static boost::asio::awaitable<void> cleanup_owned(std::shared_ptr<child>, std::chrono::steady_clock::time_point);
   static boost::asio::awaitable<void> exchange_io(std::shared_ptr<child>, std::vector<std::uint8_t>);
   static boost::asio::awaitable<compiler_protocol::header> read_header(std::shared_ptr<child>,
                                                                        compiler_protocol::message_kind);
   void adopt();
   void start_reap();
   void reap_completed(boost::system::error_code);
   void arm_deadline(std::chrono::steady_clock::time_point);
   void observe_expired_socket() noexcept;
   void append_deadline_context() noexcept;
   std::exception_ptr transport_failure(std::exception_ptr) const noexcept;
   void send_signal(int);
   void abort_io() noexcept;
   void refresh_quiescent() noexcept;
   bool drained() const noexcept;
   bool process_drained() const noexcept;
   bool is_quiescent() const noexcept;
   void remove_workspace(); // existing compute boundary, only after physical drain

   const session_options& options;
   compiler_protocol::exchange_id exchange;
   crypto::digest::sha256 identity;
   compiler_protocol::hello expected;
   bool preflight = false;
   boost::asio::strand<boost::asio::any_io_executor> strand;
   boost::asio::local::stream_protocol::socket socket;
   boost::asio::steady_timer timer;
   asio::notification changed;
   std::unique_ptr<boost::process::v2::process_handle> process;
   std::unique_ptr<std::stop_callback<std::function<void()>>> stop_registration;
   // Nonthrowing one-shot ownership publication across compute completion.
   // Actor/reap/timer/IO fields below are strand-owned only.
   std::atomic<pid_t> launched_pid{-1};
   std::atomic<int> launched_endpoint{-1};
   pid_t pid = -1;
   int parent_endpoint = -1;
   std::atomic_bool quiescent{true};
   std::atomic_bool workspace_pending{false};
   std::string workspace;
   std::atomic_bool stop_requested{false};
   int native_status = W_STOPCODE(SIGSTOP);
   bool owned = false;
   bool adopted = false;
   bool cleanup_requested = false;
   bool activation_pending = true;
   bool unresolved = false;
   bool reap_inflight = false;
   bool io_inflight = false;
   std::size_t timers_inflight = 0;
   std::uint64_t timer_generation = 0;
   bool incomplete = false;
   bool term_sent = false;
   std::chrono::steady_clock::time_point deadline;
   // Fixed scalar/time storage. Only the strand writes progress, first-arm and
   // expiry values; constructor supplies the existing initial clock sample.
   deadline_phase progress = deadline_phase::before_io;
   deadline_phase expired_phase = deadline_phase::before_io;
   std::chrono::steady_clock::time_point diagnostic_started;
   std::chrono::steady_clock::time_point diagnostic_armed;
   std::chrono::steady_clock::time_point diagnostic_expired;
   bool diagnostic_armed_once = false;
   bool diagnostic_captured = false;
   bool diagnostic_context_attempted = false;
   bool expired_io_inflight = false;
   bool expired_reap_inflight = false;
   bool expired_owned = false;
   socket_observation expired_socket = socket_observation::not_observed;
   int expired_socket_errno = 0;
   std::exception_ptr failure;
   std::exception_ptr cancelled_error;
   std::exception_ptr timeout_error;
   std::exception_ptr unavailable_error;
   std::exception_ptr allocation_error;
   std::vector<std::uint8_t> output;
};
