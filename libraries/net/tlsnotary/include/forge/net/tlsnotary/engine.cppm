module;

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>

export module forge.net.tlsnotary.engine;

export import forge.net.tlsnotary.types;
export import forge.net.tlsnotary.exceptions;
import forge.asio.runtime;
import forge.asio.compute;

export namespace forge::net::tlsnotary {

class engine {
 public:
   engine(forge::asio::runtime& runtime, forge::asio::compute::executor compute, engine_options options);
   ~engine();
   engine(const engine&) = delete;
   engine& operator=(const engine&) = delete;
   // Explicit session stop tokens/cancel(id) control cancellation. Ambient
   // coroutine cancellation cannot detach an accepted resource owner.
   boost::asio::awaitable<result> prove(prover_request request);
   boost::asio::awaitable<result> verify(verifier_request request);
   bool cancel(std::uint64_t id) noexcept;
   void request_stop() noexcept;
   // Caller keeps runtime and compute alive until this barrier completes.
   boost::asio::awaitable<void> shutdown();
   [[nodiscard]] metrics snapshot() const;

 private:
   struct impl;
   std::shared_ptr<impl> _impl;
};
} // namespace forge::net::tlsnotary
