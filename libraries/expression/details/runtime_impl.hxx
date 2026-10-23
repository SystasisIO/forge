#pragma once

namespace forge::expression {

struct runtime::impl {
   impl(options, forge::asio::runtime*);
   ~impl();
   impl(const impl&) = delete;
   impl& operator=(const impl&) = delete;

   struct outcome {
      worker_protocol::response reply;
      std::exception_ptr error;
   };
   struct admission {
      std::optional<forge::asio::gate::ticket> ticket;
      std::exception_ptr error;
   };

   void initialize_sync();
   void shutdown_sync() noexcept;
   void require_host() const;
   boost::asio::awaitable<void> initialize();
   boost::asio::awaitable<void> shutdown();
   boost::asio::awaitable<void> reap();
   void stop() noexcept;
   void launch();
   bool exhausted() noexcept;
   void transport_check(boost::system::error_code);
   boost::asio::awaitable<admission> acquire();
   boost::asio::awaitable<outcome> transfer(worker_protocol::request&, std::size_t);
   boost::asio::awaitable<worker_protocol::response> invoke(worker_protocol::request, const forge::variant_object&,
                                                            std::uint64_t* accepted_program = nullptr,
                                                            operation_options = {});
   static boost::asio::awaitable<worker_protocol::response> submit(std::shared_ptr<impl>, worker_protocol::request,
                                                                   const forge::variant_object&,
                                                                   std::uint64_t* = nullptr, operation_options = {});

   void release(std::uint64_t) noexcept;
   boost::asio::awaitable<void> release_pending();
   void release_complete(std::exception_ptr) noexcept;

   options configuration;
   std::unique_ptr<forge::asio::runtime> owned_executor;
   forge::asio::runtime& executor;
   boost::asio::strand<boost::asio::io_context::executor_type> strand;
   boost::asio::local::stream_protocol::socket socket;
   forge::asio::gate writer;
   forge::asio::gate closer;
   forge::asio::gate reaper;
   forge::asio::notification changed;
   forge::asio::child_signal_registration signal_owner;
   std::unique_ptr<boost::process::v2::process_handle> process;
   std::array<std::uint8_t, worker_protocol::frame_limit> buffer{};
   std::atomic<std::size_t> pending{0};
   std::atomic<bool> stopped{false};
   std::atomic<bool> closed{false};
   std::mutex release_mutex;
   std::array<std::uint64_t, 64> releases{};
   std::size_t release_count = 0;
   bool release_scheduled = false;
   bool release_failed = false;
   pid_t pid = -1;
   int channel = -1;
   int faults = -1;
   std::uint64_t sequence = 0;
   bool saw_exhaustion = false;
};

} // namespace forge::expression
