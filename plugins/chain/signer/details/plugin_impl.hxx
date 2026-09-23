#pragma once

#include "runtime_state.hxx"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace forge::plugins::chain::signer {

struct plugin::impl {
   explicit impl(plugin_options value);
   ~impl();

   void set_config(config value);
   void initialize();
   boost::asio::awaitable<admission_lease> acquire(std::string caller, std::size_t bytes);
   void request_stop() noexcept;
   boost::asio::awaitable<void> wait_for_drain();
   [[nodiscard]] bool stopping() const noexcept;

   [[nodiscard]] signing_policy::transaction_selection
   select_transaction(const forge::chain::transaction::unsigned_transaction& transaction,
                      const forge::api::auth::authenticated_caller& caller) const;
   [[nodiscard]] signing_policy::block_selection
   select_block(const forge::chain::protocol::block_sign_request& request,
                const forge::api::auth::authenticated_caller& caller) const;
   [[nodiscard]] signing_policy::finality_selection select_finality() const;
   void audit(audit_entry value) const noexcept;

 private:
   [[nodiscard]] std::shared_ptr<const runtime_state> runtime_snapshot() const;

   plugin_options options_;
   mutable std::mutex lifecycle_mutex_;
   std::shared_ptr<const runtime_state> runtime_;
   std::atomic_bool stop_requested_ = false;
};

} // namespace forge::plugins::chain::signer
