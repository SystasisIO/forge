module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

export module forge.swarm.runtime.engine;

export import forge.swarm.runtime.admission;
export import forge.swarm.runtime.capabilities;
export import forge.swarm.core.store;
export import forge.swarm.core.transfer;
export import forge.swarm.core.choker;
export import forge.asio.task;
export import forge.asio.compute;
export import :sharing;
export import :download;

export namespace forge::swarm::runtime {

struct resource_limits {
   std::size_t calls = 32;
   std::size_t items = 128;
   std::uint64_t bytes = 64U * 1024U * 1024U;
   std::size_t pieces = 16;
};

class engine final : public api::peer {
 public:
   struct options {
      resource_limits global;
      resource_limits per_operation{8, 32, 32U * 1024U * 1024U, 8};
      resource_limits per_peer{4, 16, 32U * 1024U * 1024U, 2};
      std::size_t operations = 16;
      std::size_t peers = 4;
      std::uint32_t attempts = 3;
      std::chrono::milliseconds idle_timeout{30000};
      std::chrono::milliseconds callback_timeout{5000};
      std::uint32_t integrity_failures = 3;
      core::transfer::config transfer;
      core::choker::config upload;
      std::chrono::milliseconds upload_round{1000};
   };
   engine(forge::asio::task::scheduler&, forge::asio::compute::executor cpu,
          std::shared_ptr<admission>, std::shared_ptr<capabilities> = {});
   engine(forge::asio::task::scheduler&, forge::asio::compute::executor cpu,
          std::shared_ptr<admission>, std::shared_ptr<capabilities>, options);
   ~engine() override;
   engine(const engine&) = delete;
   engine& operator=(const engine&) = delete;

   boost::asio::awaitable<sharing> async_share(core::descriptor, std::shared_ptr<core::store>, std::string realm = {});
   boost::asio::awaitable<runtime::download> async_download(
      core::descriptor, std::shared_ptr<core::store>, std::string realm = {},
      std::vector<std::string> providers = {},
      std::optional<std::chrono::steady_clock::time_point> total_deadline = std::nullopt);
   void cancel() noexcept;
   boost::asio::awaitable<void> async_close();
   boost::asio::awaitable<void> metadata(protocol::metadata_request,
      forge::api::auth::authenticated_caller, forge::api::core::stream_writer<protocol::metadata_chunk>) override;
   boost::asio::awaitable<void> exchange(protocol::exchange_request,
      forge::api::auth::authenticated_caller,
      forge::api::core::duplex_stream<protocol::peer_message, protocol::peer_message>) override;

 private:
   struct impl;
   std::shared_ptr<impl> _impl;
   static boost::asio::awaitable<sharing> share_owned(
      std::shared_ptr<impl>, core::descriptor, std::shared_ptr<core::store>, std::string);
   static boost::asio::awaitable<runtime::download> download_owned(
      std::shared_ptr<impl>, core::descriptor, std::shared_ptr<core::store>, std::string,
      std::vector<std::string>, std::optional<std::chrono::steady_clock::time_point>);
};

} // namespace forge::swarm::runtime
