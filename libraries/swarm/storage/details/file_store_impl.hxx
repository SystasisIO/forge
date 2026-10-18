#pragma once

namespace forge::swarm::storage {

struct file_store::impl : std::enable_shared_from_this<impl> {
   struct writer;

   struct cancellation {
      cancellation(const std::shared_ptr<impl>&, std::stop_token);
      std::stop_source source;
      std::stop_callback<std::function<void()>> caller;
      std::stop_callback<std::function<void()>> owner;
   };

   struct maintenance_request {
      explicit maintenance_request(std::shared_ptr<impl>);
      ~maintenance_request();
      maintenance_request(const maintenance_request&) = delete;
      maintenance_request& operator=(const maintenance_request&) = delete;
      std::shared_ptr<impl> owner;
   };

   struct pending_piece {
      std::uint64_t generation = 0;
      bool owned = false;
      bool busy = false;
      std::vector<std::uint8_t> blocks;
      std::uint64_t busy_generation = 0;
   };

   struct operation {
      operation(std::shared_ptr<impl>, core::piece_id, bool writing, std::uint64_t generation, std::stop_token);
      ~operation();
      operation(const operation&) = delete;
      operation& operator=(const operation&) = delete;
      std::shared_ptr<impl> owner;
      core::piece_id piece;
      bool writing;
      std::uint64_t reservation = 0;
      cancellation cancel;
   };

   explicit impl(options);
   ~impl();
   [[nodiscard]] std::uint32_t ordinal(core::piece_id) const;
   [[nodiscard]] core::availability snapshot() const;
   [[nodiscard]] std::unique_ptr<core::store::writer> begin(core::piece_id);
   void discard(core::piece_id);
   [[nodiscard]] std::shared_ptr<void> lease();
   void release_writer(core::piece_id, std::uint64_t) noexcept;
   void request_stop() noexcept;
   void check_stop(std::stop_token) const;
   void changed_locked() noexcept;
   void set_verified_locked(core::piece_id, bool);
   [[nodiscard]] core::resume_state checkpoint_locked(std::uint64_t) const;
   [[nodiscard]] bool matches(core::piece_id, std::span<const std::uint8_t>) const;

   static boost::asio::awaitable<void> initialize(std::shared_ptr<impl>, std::stop_token);
   static boost::asio::awaitable<std::vector<std::uint8_t>> read(
      std::shared_ptr<impl>, core::piece_id, std::uint32_t, std::uint32_t, std::stop_token);
   static boost::asio::awaitable<void> write(
      std::shared_ptr<impl>, core::piece_id, std::uint64_t, std::uint32_t, std::vector<std::uint8_t>, std::stop_token);
   static boost::asio::awaitable<void> commit(std::shared_ptr<impl>, core::piece_id, std::uint64_t, std::stop_token);
   static boost::asio::awaitable<void> discard_writer(std::shared_ptr<impl>, core::piece_id, std::uint64_t);
   static boost::asio::awaitable<core::availability> wait_change(std::shared_ptr<impl>, std::uint64_t, std::stop_token);
   static boost::asio::awaitable<void> quiesce(std::shared_ptr<impl>, bool closing, std::stop_token);
   static boost::asio::awaitable<core::resume_state> save(std::shared_ptr<impl>, std::stop_token);
   static boost::asio::awaitable<core::resume_state> checkpoint(std::shared_ptr<impl>, std::stop_token);
   static boost::asio::awaitable<void> publish(std::shared_ptr<impl>, std::stop_token);
   static boost::asio::awaitable<void> close(std::shared_ptr<impl>);
   static boost::asio::awaitable<void> close_owned(std::shared_ptr<impl>);

   options config;
   core::swarm_id id;
   std::vector<std::uint32_t> offsets;
   std::unique_ptr<detail::file_layout> layout;
   mutable std::mutex mutex;
   std::map<core::piece_id, pending_piece> pending;
   std::map<core::piece_id, std::uint32_t> readers;
   std::vector<std::uint8_t> verified;
   std::uint64_t generation = 1;
   std::uint64_t durable_generation = 0;
   std::uint64_t next_checkpoint = 0;
   std::uint64_t next_writer = 0;
   std::uint64_t next_operation = 0;
   std::size_t active = 0;
   std::size_t leases = 0;
   std::size_t maintenance_waiters = 0;
   bool maintenance = false;
   bool closing = false;
   bool closed = false;
   bool published = false;
   std::stop_source stop;
   forge::asio::gate maintenance_gate;
   forge::asio::notification changed;
};

} // namespace forge::swarm::storage
