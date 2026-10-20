module;

#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>

export module forge.swarm.runtime.engine:sharing;

export import forge.swarm.runtime.quota;

namespace forge::swarm::runtime::detail { struct operation_state; }

export namespace forge::swarm::runtime {

struct operation_status {
   facts traffic;
   std::size_t active_calls = 0;
   std::size_t upload_candidates = 0;
   std::size_t unchoked_peers = 0;
   std::size_t capacity_waiters = 0;
   std::uint64_t capacity_waits = 0;
   bool complete = false;
   bool canceled = false;
   bool closed = false;
};

class engine;

class sharing {
 public:
   sharing();
   ~sharing();
   sharing(sharing&&) noexcept;
   sharing& operator=(sharing&&) noexcept;
   sharing(const sharing&) = delete;
   sharing& operator=(const sharing&) = delete;
   void cancel() noexcept;
   [[nodiscard]] operation_status inspect() const;
   boost::asio::awaitable<void> async_close();
 private:
   explicit sharing(std::shared_ptr<detail::operation_state>);
   std::shared_ptr<detail::operation_state> _state;
   friend class engine;
};

} // namespace forge::swarm::runtime
