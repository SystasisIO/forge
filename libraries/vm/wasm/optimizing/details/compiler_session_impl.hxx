#pragma once

// One owning session component. This header is included after the owning
// module declaration/imports; it does not export Boost.Process or OS types.
namespace forge::vm::wasm::optimizing {

struct compiler_session::impl {
   struct accounting {
      std::mutex mutex;
      std::uint64_t inflight_bytes = 0;
      std::uint64_t resident_bytes = 0;
      std::size_t tickets = 0;
   };
   struct child;
   enum class initialization_phase { cold, initializing, ready, failed };
   struct initialization {
      target_profile target;
      std::stop_source stop;
      std::unique_ptr<std::stop_callback<std::function<void()>>> registration;
      std::atomic_bool submitted{false};
      asio::notification changed;
      std::mutex mutex; // waiter outcome owns its lifetime independently of the session
      std::exception_ptr error;
      std::exception_ptr cancelled_error;
      std::uint64_t charged_bytes = 0; // conservative reservation lives through the last outcome pin
      bool completing = false;
      bool finalized = false; // index-mutex owned; only owner joins registration/releases live record
      bool completed = false; // initialization mutex owned; terminal waiter outcome
      void cancel() noexcept; // outcome publication is NOT live-record retirement
   };
   struct disk_entry {
      enum class state { candidate, quarantined, opaque };
      char name[256]{};
      std::uint64_t bytes = 0;
      std::uint64_t device = 0;
      std::uint64_t inode = 0;
      std::int64_t modified_seconds = 0;
      std::int64_t modified_nanoseconds = 0;
      state status = state::opaque;
   };
   struct job {
      compiler_protocol::exchange_id exchange;
      crypto::digest::sha256 hint;
      std::optional<crypto::digest::sha256> verified;
      std::optional<compile_request> request;
      std::chrono::steady_clock::time_point queued_time;
      asio::task::handle handle;
      std::unique_ptr<std::weak_ptr<ticket::state>[]> consumers;
      std::shared_ptr<child> process;
      std::shared_ptr<initialization> setup; // tracked control, never a fake artifact/ticket
      // The SAME record owns a prepared result across compute's post-work
      // cancellation checkpoint; no mapping is hidden in a canceled result.
      std::optional<artifact> prepared;
      std::uint64_t charged_bytes = 0;
      std::uint64_t resident_bytes = 0;
   };
   struct entry {
      crypto::digest::sha256 identity;
      compile_configuration configuration;
      std::shared_ptr<job> pending;
      std::optional<completion> result;
      std::uint64_t charged_bytes = 0;
   };

   impl(asio::runtime&, asio::compute::executor, session_options);
   ~impl();
   boost::asio::awaitable<void> initialize(target_profile, std::stop_token);
   static boost::asio::awaitable<void> wait_initialize(std::shared_ptr<initialization>);
   boost::asio::awaitable<void> run_initialize(std::shared_ptr<job>, std::stop_token);
   void finish_initialize(const std::shared_ptr<job>&, std::exception_ptr);
   void acquire_signal_owner();
   void release_signal_owner();
#if defined(__APPLE__)
   static std::mutex signal_mutex;
   static std::size_t signal_registrations;
   static std::optional<struct sigaction> signal_disposition;
   static bool same_signal_disposition(const struct sigaction&) noexcept;
#endif
   std::optional<artifact> ready(const crypto::digest::sha256&, const compile_configuration&) const;
   enqueue_result enqueue(const crypto::digest::sha256&, compile_request&&, asio::task::priority);
   void request_stop();
   boost::asio::awaitable<void> shutdown(std::chrono::steady_clock::time_point);

   static std::optional<std::uint64_t> configuration_bytes(const compile_configuration&,
                                                           const session_options&) noexcept;
   static std::optional<std::uint64_t> request_bytes(const compile_request&, const session_options&) noexcept;
   boost::asio::awaitable<void> run(std::shared_ptr<job>, std::stop_token);
   boost::asio::awaitable<std::vector<std::uint8_t>> exchange(std::shared_ptr<job>, std::vector<std::uint8_t>,
                                                              std::stop_token);
   boost::asio::awaitable<std::optional<artifact>> load(std::shared_ptr<job>, std::stop_token);
   boost::asio::awaitable<artifact> prepare(std::shared_ptr<job>, std::vector<std::uint8_t>, std::stop_token);
   boost::asio::awaitable<void> persist(std::shared_ptr<job>, std::stop_token);
   void initialize_cache(const target_profile&, asio::compute::context&);
   void scan_cache(const target_profile&, asio::compute::context&);
   void require_cache_space(std::uint64_t, std::size_t, asio::compute::context&, const char* preserved = nullptr);
   void remove_cache_entry(std::size_t, asio::compute::context&);
   std::optional<std::vector<std::uint8_t>> read_cache_entry(std::size_t, asio::compute::context&);
   bool cache_entry_matches(std::size_t, int fd = -1) const noexcept;
   void touch_cache_entry(const crypto::digest::sha256&, asio::compute::context&);
   void verify_cache_ownership() const;
   void close_cache() noexcept;
   void finish(const std::shared_ptr<job>&, std::optional<completion>, std::exception_ptr);
   void release_job(const std::shared_ptr<job>&);
   bool reserve_resident(std::uint64_t);
   bool evict_one();
   void release_resident(std::uint64_t) noexcept;

   asio::runtime& runtime;
   asio::compute::executor compute;
   session_options options;
   asio::task::scheduler scheduler;
   std::shared_ptr<accounting> account;
   mutable std::mutex index_mutex;
   // Exact bounded slots: an index, not an ordered second work queue.
   std::unique_ptr<std::optional<entry>[]> index;
   std::unique_ptr<std::shared_ptr<job>[]> live;
   std::size_t live_slots = 0;
   std::uint64_t generation = 0;
   std::uint64_t next_job = 1;
   std::uint64_t workspace_bytes = 0;
   std::exception_ptr cancelled_error;
   std::exception_ptr allocation_error;
   bool stopped = false;
   bool quiescent = true;
   bool shutdown_active = false;
   initialization_phase phase = initialization_phase::cold;
   std::optional<target_profile> initialized_target;
   // Only existing compute work holds this cold filesystem mutex. Index/writer
   // and Asio handlers never wait on it. No separate cache/work queue.
   std::mutex disk_mutex;
   std::unique_ptr<disk_entry[]> disk;
   std::size_t disk_count = 0;
   std::uint64_t disk_bytes = 0;
   bool disk_faulted = false; // failed unlink/sync retains charges; no subsequent write/reconcile
   int directory_fd = -1;
   int ownership_fd = -1;
   // A cold native Asio registration, never a child waiter/reaper. SAME-owned
   // Darwin sessions share Asio's process signal registration, each on its
   // injected runtime. Checked clear must succeed before this owner is retired.
   std::unique_ptr<boost::asio::signal_set> signal_owner;
};

} // namespace forge::vm::wasm::optimizing
