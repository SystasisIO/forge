module;

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

export module forge.swarm.core.store;

export import forge.swarm.core.checkpoint;

export namespace forge::swarm::core {

struct availability {
   std::uint64_t generation = 0;
   std::uint64_t durable_generation = 0;
   bool published = false;
   std::vector<std::uint8_t> verified;
   std::vector<unfinished_piece> unfinished;
};

class checkpoint_store {
 public:
   virtual ~checkpoint_store();
   virtual boost::asio::awaitable<std::optional<resume_state>> async_load(swarm_id, std::stop_token) = 0;
   virtual boost::asio::awaitable<void> async_save(resume_state, std::stop_token) = 0;
};

class store {
 public:
   class writer;
   class lease;

   virtual ~store();
   [[nodiscard]] virtual availability snapshot() const = 0;
   [[nodiscard]] virtual std::unique_ptr<writer> begin(piece_id) = 0;
   virtual void discard(piece_id) = 0;
   [[nodiscard]] virtual lease acquire_lease() = 0;
   virtual boost::asio::awaitable<std::vector<std::uint8_t>> async_read(
      piece_id, std::uint32_t offset, std::uint32_t length, std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<availability> async_wait_change(std::uint64_t generation, std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<resume_state> async_checkpoint(std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<void> async_publish(std::stop_token = {}) = 0;
   virtual void request_stop() noexcept = 0;
   virtual boost::asio::awaitable<void> async_close() = 0;

 protected:
   [[nodiscard]] static lease make_lease(std::shared_ptr<void>);
};

class store::writer {
 public:
   virtual ~writer();
   [[nodiscard]] virtual piece_id piece() const noexcept = 0;
   virtual boost::asio::awaitable<void> async_write(std::uint32_t offset, std::vector<std::uint8_t>, std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<void> async_commit(std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<void> async_discard() = 0;
};

class store::lease {
 public:
   lease();
   ~lease();
   lease(lease&&) noexcept;
   lease& operator=(lease&&) noexcept;
   lease(const lease&) = delete;
   lease& operator=(const lease&) = delete;
   void release() noexcept;
   [[nodiscard]] bool active() const noexcept;

 private:
   explicit lease(std::shared_ptr<void>);
   std::shared_ptr<void> _guard;
   friend class store;
};

} // namespace forge::swarm::core
