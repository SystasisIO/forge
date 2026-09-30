module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.crypto.wallet.provider;

import forge.api.core.exceptions;
import forge.asio.gate;
import forge.asio.exceptions;
import forge.asio.task;
import forge.asio.compute;
import forge.crypto.asymmetric;
import forge.crypto.keystore.store;
import forge.crypto.wallet.exceptions;

#include "details/provider_impl.hxx"

namespace forge::plugins::crypto::wallet {
namespace management = forge::crypto::wallet;
namespace keystore = forge::crypto::keystore;
namespace signer = forge::crypto::signer;
namespace asymmetric = forge::crypto::asymmetric;

provider::provider(std::string name) : impl_{std::make_shared<impl>(std::move(name))} {}
provider::~provider() {
   request_stop();
}

const std::string& provider::name() const noexcept {
   return impl_->name;
}

void provider::initialize(std::filesystem::path directory, forge::asio::task::scheduler& scheduler,
                          forge::asio::compute::executor compute, std::uint32_t timeout_seconds,
                          std::uint32_t max_pending) {
   if (impl_->scheduler || !compute.valid() || directory.empty() || max_pending == 0 || timeout_seconds > 86400) {
      FORGE_THROW_EXCEPTION(management::exceptions::invalid_request, "Invalid wallet initialization");
   }
   impl_->path = std::move(directory) / (impl_->name + ".fks");
   impl_->scheduler = &scheduler;
   impl_->compute = std::move(compute);
   impl_->timeout = timeout_seconds;
   impl_->max_pending = max_pending;
}

void provider::startup() {
   impl_->require_initialized();
   if (impl_->started) {
      FORGE_THROW_EXCEPTION(management::exceptions::invalid_request, "Wallet provider was already started");
   }
   impl_->started = true;
}

void provider::request_stop() noexcept {
   impl_->stopping.store(true, std::memory_order_release);
   impl_->accepting_signatures.store(false, std::memory_order_release);
}

boost::asio::awaitable<void> provider::shutdown() {
   request_stop();
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto ticket = co_await boost::asio::co_spawn(
       co_await boost::asio::this_coro::executor, impl_->gate.acquire(),
       boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
   impl_->store.reset();
   impl_->owner.reset();
   impl_->state = management::state::locked;
   auto timer = std::move(impl_->timer);
   timer.cancel();
   ++impl_->timer_generation;
   impl_->timer_scheduled = false;
   impl_->gate.close();
   ticket.release();
   if (timer.accepted()) {
      try {
         co_await timer.wait();
      } catch (const forge::asio::exceptions::canceled&) {
         // A pending deadline was canceled, not an unfinished key operation.
      }
   }
}

boost::asio::awaitable<management::wallet_status> provider::create(forge::crypto::core::secret_string password) {
   auto state = impl_;
   auto ticket = co_await state->enter();
   if (state->owner) {
      FORGE_THROW_EXCEPTION(management::exceptions::already_exists, "Wallet is already open");
   }
   state->owner = co_await state->blocking([state, password = std::move(password)]() mutable {
      auto store = keystore::store::create(state->path, std::move(password));
      return store.owner();
   });
   state->state = management::state::locked;
   co_return state->snapshot();
}

boost::asio::awaitable<management::wallet_status> provider::open() {
   auto state = impl_;
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      throw forge::api::core::exceptions::cancelled{"Wallet operation was canceled"};
   }
   state->reserve();
   state->accepting_signatures.store(false, std::memory_order_release);
   state->lock_generation.fetch_add(1, std::memory_order_acq_rel);
   // Once revocation is visible, finish draining and locking even if the
   // transport disappears. Queue refusal above has not changed wallet state.
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto ticket = co_await boost::asio::co_spawn(
       co_await boost::asio::this_coro::executor, state->enter_reserved(),
       boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
   if (state->state == management::state::fault) {
      FORGE_THROW_EXCEPTION(management::exceptions::storage_error, "Wallet requires operator recovery");
   }
   state->store.reset();
   state->state = management::state::locked;
   state->cancel_expiry();
   auto owner = state->owner;
   state->owner = co_await state->blocking([state, owner = std::move(owner)] {
      auto acquired = owner ? owner : keystore::ownership::acquire(state->path);
      acquired->require_file(keystore::decrypt_limits{}.max_plaintext_bytes + 4096);
      return acquired;
   });
   co_return state->snapshot();
}

boost::asio::awaitable<management::wallet_status> provider::status() {
   auto state = impl_;
   auto ticket = co_await state->enter();
   co_return state->snapshot();
}

boost::asio::awaitable<management::wallet_status> provider::unlock(forge::crypto::core::secret_string password) {
   auto state = impl_;
   const auto generation = state->lock_generation.load(std::memory_order_acquire);
   auto ticket = co_await state->enter();
   if (state->state == management::state::fault) {
      FORGE_THROW_EXCEPTION(management::exceptions::storage_error, "Wallet requires operator recovery");
   }
   if (!state->owner) {
      FORGE_THROW_EXCEPTION(management::exceptions::not_found, "Wallet is not open");
   }
   if (state->store) {
      FORGE_THROW_EXCEPTION(management::exceptions::already_exists, "Wallet is already unlocked");
   }
   auto opened = co_await state->blocking([owner = state->owner, password = std::move(password)]() mutable {
      return keystore::store::open(std::move(owner), std::move(password));
   });
   if (generation == state->lock_generation.load(std::memory_order_acquire) &&
       !state->stopping.load(std::memory_order_acquire)) {
      state->store.emplace(std::move(opened));
      state->state = management::state::unlocked;
      state->accepting_signatures.store(true, std::memory_order_release);
      state->refresh();
   }
   co_return state->snapshot();
}

boost::asio::awaitable<management::wallet_status> provider::lock() {
   auto state = impl_;
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      throw forge::api::core::exceptions::cancelled{"Wallet operation was canceled"};
   }
   state->reserve();
   state->accepting_signatures.store(false, std::memory_order_release);
   state->lock_generation.fetch_add(1, std::memory_order_acq_rel);
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto ticket = co_await boost::asio::co_spawn(
       co_await boost::asio::this_coro::executor, state->enter_reserved(),
       boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
   state->store.reset();
   state->cancel_expiry();
   if (state->state != management::state::fault) {
      state->state = management::state::locked;
   }
   co_return state->snapshot();
}

boost::asio::awaitable<management::wallet_status> provider::set_timeout(std::uint32_t seconds) {
   if (seconds > 86400) {
      FORGE_THROW_EXCEPTION(management::exceptions::invalid_request, "Wallet timeout exceeds one day");
   }
   auto state = impl_;
   auto ticket = co_await state->enter();
   state->timeout = seconds;
   state->refresh();
   co_return state->snapshot();
}

boost::asio::awaitable<management::key_info> provider::create_key(std::string id) {
   if (id.empty() || id.size() > keystore::store_options{}.max_key_id_bytes || id.find('\0') != std::string::npos) {
      FORGE_THROW_EXCEPTION(management::exceptions::invalid_request, "Invalid key identifier");
   }
   auto state = impl_;
   auto ticket = co_await state->enter();
   state->require_unlocked();
   auto result = co_await state->blocking([state, id = std::move(id)] {
      auto key = asymmetric::private_key::generate();
      auto result = management::key_info{.id = id, .key = asymmetric::encoding::forge().format(key.get_public_key())};
      state->store->put({.value = id}, std::move(key));
      return result;
   });
   state->refresh();
   co_return result;
}

boost::asio::awaitable<management::key_info> provider::import_key(std::string id,
                                                                  forge::crypto::core::secret_string key) {
   if (id.empty() || id.size() > keystore::store_options{}.max_key_id_bytes || id.find('\0') != std::string::npos) {
      FORGE_THROW_EXCEPTION(management::exceptions::invalid_request, "Invalid key identifier");
   }
   auto state = impl_;
   auto ticket = co_await state->enter();
   state->require_unlocked();
   auto result = co_await state->blocking([state, id = std::move(id), key = std::move(key)] {
      auto parsed = asymmetric::encoding::forge().parse_private(key.view());
      auto result =
          management::key_info{.id = id, .key = asymmetric::encoding::forge().format(parsed.get_public_key())};
      state->store->put({.value = id}, std::move(parsed));
      return result;
   });
   state->refresh();
   co_return result;
}

boost::asio::awaitable<management::wallet_status> provider::remove_key(std::string id) {
   if (id.empty() || id.size() > keystore::store_options{}.max_key_id_bytes || id.find('\0') != std::string::npos) {
      FORGE_THROW_EXCEPTION(management::exceptions::invalid_request, "Invalid key identifier");
   }
   auto state = impl_;
   auto ticket = co_await state->enter();
   state->require_unlocked();
   static_cast<void>(co_await state->blocking([state, id = std::move(id)] {
      state->store->erase({.value = id});
      return true;
   }));
   state->refresh();
   co_return state->snapshot();
}

boost::asio::awaitable<std::vector<signer::key_info>> provider::keys() {
   auto state = impl_;
   auto ticket = co_await state->enter();
   state->require_unlocked();
   co_return co_await state->blocking([state] { return state->store->keys_sync(); });
}

boost::asio::awaitable<signer::key_info> provider::describe(const signer::key_id& id) {
   auto state = impl_;
   auto ticket = co_await state->enter();
   state->require_unlocked();
   co_return co_await state->blocking([state, id] { return state->store->describe_sync(id); });
}

boost::asio::awaitable<signer::sign_digest_response> provider::sign_digest(signer::sign_digest_request request) {
   auto state = impl_;
   if (!state->accepting_signatures.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(management::exceptions::locked, "Wallet is locked");
   }
   auto ticket = co_await state->enter();
   state->require_unlocked();
   auto result = co_await state->blocking(
       [state, request = std::move(request)] { return state->store->sign_digest_sync(request); });
   state->refresh();
   co_return result;
}

} // namespace forge::plugins::crypto::wallet
