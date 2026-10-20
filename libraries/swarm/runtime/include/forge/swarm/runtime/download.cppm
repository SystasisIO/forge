module;

#include <boost/asio/awaitable.hpp>
#include <memory>

export module forge.swarm.runtime.engine:download;

import :sharing;

export namespace forge::swarm::runtime {

class download {
 public:
   download();
   ~download();
   download(download&&) noexcept;
   download& operator=(download&&) noexcept;
   download(const download&) = delete;
   download& operator=(const download&) = delete;
   void cancel() noexcept;
   [[nodiscard]] operation_status inspect() const;
   boost::asio::awaitable<void> async_wait();
   boost::asio::awaitable<void> async_close();
 private:
   explicit download(std::shared_ptr<detail::operation_state>);
   std::shared_ptr<detail::operation_state> _state;
   friend class engine;
};

} // namespace forge::swarm::runtime
