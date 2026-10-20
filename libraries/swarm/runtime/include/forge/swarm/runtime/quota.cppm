module;

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>

export module forge.swarm.runtime.quota;

export import forge.swarm.api.peer;

export namespace forge::swarm::runtime {

// Transport facts, not receipts or billable units. Monotonic per grant/operation.
struct facts {
   std::uint64_t requested = 0;
   std::uint64_t attempted = 0;
   std::uint64_t queued = 0;
   std::uint64_t uncertain = 0;
   std::uint64_t sent = 0;
   std::uint64_t received = 0;
   std::uint64_t verified = 0;
};

class quota {
 public:
   class reservation;
   struct options {
      core::swarm_id content;
      forge::api::auth::authenticated_caller requester;
      std::uint64_t bytes = 0;
      std::chrono::steady_clock::time_point expires = std::chrono::steady_clock::time_point::max();
   };
   struct status {
      facts traffic;
      std::uint64_t available = 0;
      std::uint64_t held = 0;
      std::uint64_t finalized = 0;
      bool revoked = false;
   };

   explicit quota(options);
   ~quota();
   quota(const quota&) = delete;
   quota& operator=(const quota&) = delete;
   [[nodiscard]] bool permits(core::swarm_id, const forge::api::auth::authenticated_caller&) const;
   [[nodiscard]] reservation reserve(std::uint64_t bytes);
   [[nodiscard]] status inspect() const;
   [[nodiscard]] std::stop_token stop_token() const noexcept;
   [[nodiscard]] std::chrono::steady_clock::time_point expires() const noexcept;
   void revoke() noexcept;

 private:
   struct impl;
   std::shared_ptr<impl> _impl;
};

class quota::reservation {
 public:
   reservation();
   ~reservation();
   reservation(reservation&&) noexcept;
   reservation& operator=(reservation&&) noexcept;
   reservation(const reservation&) = delete;
   reservation& operator=(const reservation&) = delete;
   // Attempt BEFORE async_write, confirm queued AFTER successful async_write.
   // An attempted but unconfirmed write remains charged and is uncertain.
   void queue();
   void queued();
   void sent();
   void finalize() noexcept;

 private:
   reservation(std::shared_ptr<impl>, std::uint64_t);
   std::shared_ptr<impl> _owner;
   std::uint64_t _bytes = 0;
   bool _attempted = false;
   bool _queued = false;
   bool _sent = false;
   friend class quota;
};

} // namespace forge::swarm::runtime
