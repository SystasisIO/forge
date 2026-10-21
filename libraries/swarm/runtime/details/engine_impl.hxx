#pragma once

namespace forge::swarm::runtime {

struct engine::impl : std::enable_shared_from_this<impl> {
   impl(forge::asio::task::scheduler&, forge::asio::compute::executor,
      std::shared_ptr<admission>, std::shared_ptr<capabilities>, options);
   void cancel() noexcept;
   std::shared_ptr<detail::operation_state> find(core::swarm_id, const std::string&);
   static boost::asio::awaitable<std::shared_ptr<detail::operation_state>> create(
      std::shared_ptr<impl>, core::descriptor, std::shared_ptr<core::store>, std::string,
      bool downloading, std::vector<std::unique_ptr<capabilities::route>>, std::optional<std::chrono::steady_clock::time_point>);
   static boost::asio::awaitable<void> close(std::shared_ptr<impl>);
   static std::uint64_t initial_seed(std::optional<std::uint64_t>);
   static std::uint64_t mix_seed(std::uint64_t) noexcept;
   forge::asio::task::scheduler& scheduler;
   forge::asio::compute::executor cpu;
   std::shared_ptr<admission> policy;
   std::shared_ptr<capabilities> providers;
   options config;
   std::shared_ptr<detail::resource_budget> budget;
   std::mutex mutex;
   std::vector<std::shared_ptr<detail::operation_state>> operations;
   std::uint64_t sequence = 1;
   const std::uint64_t selection_seed;
   std::uint64_t seed_sequence = 0;
   bool stopped = false;
};

} // namespace forge::swarm::runtime
