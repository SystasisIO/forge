#pragma once

namespace forge::swarm::runtime::detail {

class peer_session;

struct operation_state : std::enable_shared_from_this<operation_state> {
   struct piece_work {
      std::unique_ptr<core::store::writer> writer;
      forge::asio::gate gate;
      resource_budget::ticket capacity;
      std::uint64_t generation = 0;
   };
   operation_state(std::uint64_t, std::uint64_t picker_seed, std::uint64_t choker_seed,
      core::descriptor, std::shared_ptr<core::store>, std::string,
      forge::asio::task::scheduler&, forge::asio::compute::executor, std::shared_ptr<admission>,
      std::shared_ptr<capabilities>, engine::options, std::shared_ptr<resource_budget>, bool,
      std::optional<std::chrono::steady_clock::time_point>);
   void cancel() noexcept;
   void check() const;
   [[nodiscard]] operation_status inspect() const;
   void add_fact(std::uint64_t facts::*, std::uint64_t);
   void enter();
   void leave() noexcept;
   [[nodiscard]] std::uint64_t next_peer();
   [[nodiscard]] std::shared_ptr<piece_work> piece(core::piece_id, const std::string& peer);
   void upload_enter(core::peer_id);
   void upload_leave(core::peer_id) noexcept;
   void upload_interest(core::peer_id, bool);
   void upload_served(core::peer_id, std::uint64_t);
   [[nodiscard]] bool upload_allowed(core::peer_id);
   void upload_select_locked();
   static boost::asio::awaitable<void> wait(std::shared_ptr<operation_state>);
   static boost::asio::awaitable<void> close(std::shared_ptr<operation_state>);
   static boost::asio::awaitable<void> run(std::shared_ptr<operation_state>, std::vector<std::string>);
   static boost::asio::awaitable<bool> publish_if_complete(std::shared_ptr<operation_state>);
   static boost::asio::awaitable<void> watch_deadline(std::shared_ptr<operation_state>);

   const std::uint64_t number;
   const std::uint64_t picker_seed;
   const std::uint64_t choker_seed;
   core::descriptor manifest;
   core::swarm_id content;
   std::uint32_t metadata_size = 0;
   const std::string realm;
   std::shared_ptr<core::store> store;
   core::store::lease lease;
   forge::asio::task::scheduler& scheduler;
   forge::asio::compute::executor cpu;
   const std::shared_ptr<admission> policy;
   const std::shared_ptr<capabilities> providers;
   const engine::options config;
   const std::shared_ptr<resource_budget> budget;
   const bool downloading;
   const std::optional<std::chrono::steady_clock::time_point> total_deadline;
   mutable std::mutex mutex;
   operation_status state;
   std::unique_ptr<core::transfer> picker;
   std::map<core::piece_id, std::shared_ptr<piece_work>> pieces;
   std::map<core::peer_id, std::weak_ptr<peer_session>> sessions;
   std::unique_ptr<core::choker> upload;
   std::map<core::peer_id, core::choker::candidate> candidates;
   std::set<core::peer_id> allowed;
   core::transfer_time upload_time{};
   std::chrono::steady_clock::time_point upload_next{};
   resource_budget::ticket metadata_capacity;
   resource_budget::ticket algorithm_capacity;
   resource_budget::ticket timer_capacity;
   // Retained after close: cancellation may snapshot its raw address unlocked.
   std::unique_ptr<capabilities::registration> registration;
   std::shared_ptr<forge::asio::task::handle> worker;
   std::shared_ptr<resource_budget::ticket> orchestration_capacity;
   std::exception_ptr failure;
   std::exception_ptr storage_failure;
   std::exception_ptr cleanup_failure;
   std::exception_ptr timer_failure;
   std::uint64_t peer_sequence = 1;
   std::uint64_t useful_progress = 0;
   std::uint32_t integrity_failures = 0;
   bool deadline_watch_done = true;
   bool initializing = true;
   bool joined = false;
   std::stop_source stop;
   forge::asio::gate join_gate;
   forge::asio::notification changed;
};

} // namespace forge::swarm::runtime::detail
