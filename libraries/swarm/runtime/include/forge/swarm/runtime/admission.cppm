module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <stop_token>

export module forge.swarm.runtime.admission;

export import forge.swarm.runtime.quota;

export namespace forge::swarm::runtime {

enum class access_kind { metadata, content };

class admission {
 public:
   virtual ~admission();
   // Product callbacks MUST honor stop/deadline; runtime joins even late completion.
   // Return the SAME grant for all sessions spending the same product budget.
   virtual boost::asio::awaitable<std::shared_ptr<quota>> async_authorize(
      core::swarm_id, forge::api::auth::authenticated_caller, access_kind, std::stop_token,
      std::chrono::steady_clock::time_point deadline) = 0;
};

// Explicit opt-in. Never selected by engine when policy is missing/fails.
class free_admission final : public admission {
 public:
   boost::asio::awaitable<std::shared_ptr<quota>> async_authorize(
      core::swarm_id, forge::api::auth::authenticated_caller, access_kind, std::stop_token,
      std::chrono::steady_clock::time_point deadline) override;
};

} // namespace forge::swarm::runtime
