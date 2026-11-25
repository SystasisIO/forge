module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/describe.hpp>
#include <boost/scope/scope_exit.hpp>
#include <forge/db/object/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

module forge.api.chain.outbox;

import forge.asio.cancellation_scope;
import forge.asio.notification;
import forge.chain.api.exceptions;
import forge.chain.api.limits;
import forge.chain.protocol.block;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.index;
import forge.db.object.object;

#include "details/outbox_attempt.hxx"

namespace forge::api::chain {
namespace {

constexpr auto service_limits = forge::chain::protocol::service_limits{};
constexpr auto http_destination_prefix = std::string_view{"http:"};

std::uint64_t now_ms() {
   const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::system_clock::now().time_since_epoch()).count();
   return count < 0 ? 0 : static_cast<std::uint64_t>(count);
}

void require_running(std::stop_token stop) {
   if (stop.stop_requested()) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::deadline_exceeded, "chain outbox operation stopped");
   }
}

std::string encode_destination(const forge::chain::protocol::chain_id& chain, const destination& target) {
   if (chain == forge::chain::protocol::chain_id{}) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request, "chain outbox requires a chain");
   }
   if (const auto* endpoint = std::get_if<http_endpoint>(&target)) {
      if (endpoint->name.empty()) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request,
                               "chain outbox requires an HTTP endpoint name");
      }
      return std::string{http_destination_prefix} + endpoint->name;
   }

   // Preserve the original peer ID text. The HTTP tag is outside the peer ID alphabet.
   const auto& peer = std::get<forge::net::p2p::peer_id>(target).value;
   if (peer.empty() || peer.starts_with(http_destination_prefix)) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request, "invalid chain outbox peer ID");
   }
   return peer;
}

destination decode_destination(std::string_view value) {
   if (value.starts_with(http_destination_prefix)) {
      value.remove_prefix(http_destination_prefix.size());
      return http_endpoint{.name = std::string{value}};
   }
   return forge::net::p2p::peer_id{std::string{value}};
}

detail::outbox_attempt make_attempt(forge::chain::protocol::chain_id chain, const destination& target,
                                    forge::chain::protocol::packed_transaction packed,
                                    outbox::delivery_policy policy) {
   auto stored_destination = encode_destination(chain, target);
   if ((policy.mode != outbox::replay::reconcile_only && policy.mode != outbox::replay::same_packed) ||
       policy.version == 0 || policy.id.size() > 128) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request, "invalid chain outbox delivery policy");
   }

   const auto canonical = policy.mode == outbox::replay::same_packed ? "same_packed" : "reconcile_only";
   if (policy.id.empty()) {
      policy.id = canonical;
   }
   if ((policy.id == "same_packed" || policy.id == "reconcile_only") &&
       (policy.id != canonical || policy.version != 1)) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request, "invalid built-in chain outbox policy");
   }

   auto request = forge::chain::protocol::transaction_submit_request{.transaction = std::move(packed)};
   forge::chain::api::require_request_within_limits(request, service_limits);
   const auto decoded = request.transaction.get_signed_transaction();
   auto attempt = detail::outbox_attempt{};
   attempt.chain = chain;
   attempt.transaction = forge::chain::protocol::calculate_transaction_id(decoded);
   attempt.peer = std::move(stored_destination);
   attempt.packed = std::move(request.transaction);
   attempt.mode = policy.mode;
   attempt.policy = std::move(policy.id);
   attempt.policy_version = policy.version;
   attempt.expires = static_cast<std::uint64_t>(decoded.expiration.sec_since_epoch()) * 1000;
   return attempt;
}

bool matches(const detail::outbox_attempt& current, const detail::outbox_attempt& expected) {
   return current.chain == expected.chain && current.transaction == expected.transaction &&
          current.peer == expected.peer && current.packed == expected.packed && current.mode == expected.mode &&
          current.policy == expected.policy && current.policy_version == expected.policy_version &&
          current.expires == expected.expires;
}

template <typename Read>
boost::asio::awaitable<std::optional<detail::outbox_attempt>>
find(Read& active, forge::chain::protocol::chain_id chain, forge::chain::protocol::transaction_id transaction) {
   co_return co_await active.template index<detail::outbox_attempt_index, detail::by_transaction>()
       .find(chain, transaction);
}

bool is_acknowledged(detail::delivery_state state) {
   return state == detail::delivery_state::acknowledged || state == detail::delivery_state::finalized;
}

void require_uncaptured(forge::db::object::transaction& active) {
   if (active.db_transaction().captures_mutations()) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request,
                            "chain outbox cannot stage in reversible revision history");
   }
}

} // namespace

struct outbox::impl {
   storage store;
   transport io;
   options settings;
   std::mutex advancing_mutex;
   std::set<std::pair<forge::chain::protocol::chain_id, forge::chain::protocol::transaction_id>> advancing;
   forge::asio::notification changed;

   impl(storage source, transport remote, options configuration)
       : store(std::move(source)), io(std::move(remote)), settings(std::move(configuration)) {}

   bool in_flight(forge::chain::protocol::chain_id chain, forge::chain::protocol::transaction_id transaction) {
      const auto lock = std::scoped_lock{advancing_mutex};
      return advancing.contains({chain, transaction});
   }

   boost::asio::awaitable<void> stage(forge::db::object::transaction& active, detail::outbox_attempt attempt,
                                    const admission& admit) {
      require_uncaptured(active);
      const auto existing = co_await find(active, attempt.chain, attempt.transaction);
      if (existing) {
         if (!matches(*existing, attempt)) {
            FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::conflict,
                                  "transaction ID has another retained chain outbox packet or policy");
         }
         co_return;
      }
      if (admit) {
         co_await admit(active, attempt.transaction, attempt.packed);
         require_uncaptured(active);
      }

      co_await active.create<detail::outbox_attempt>([&](auto& saved) {
         attempt.id = saved.id;
         saved = std::move(attempt);
      });
   }

   boost::asio::awaitable<std::optional<detail::outbox_attempt>>
   read(forge::chain::protocol::chain_id chain, forge::chain::protocol::transaction_id transaction) {
      auto snapshot = co_await store.begin_read();
      co_return co_await find(snapshot, chain, transaction);
   }

   boost::asio::awaitable<std::optional<forge::chain::protocol::transaction_status_response>>
   status(forge::chain::protocol::chain_id chain, destination target,
          forge::chain::protocol::transaction_id transaction, std::stop_token stop) {
      require_running(stop);
      auto response = forge::chain::protocol::transaction_status_response{};
      try {
         response = co_await io.status(chain, std::move(target), transaction, stop);
      } catch (const forge::chain::api::exceptions::audit_not_supported&) {
         co_return std::nullopt;
      } catch (const forge::chain::api::exceptions::history_unavailable&) {
         co_return std::nullopt;
      }

      require_running(stop);
      if (response.id != transaction || response.context.chain != chain) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_transaction_proof,
                               "chain outbox observed another transaction or chain");
      }
      if (response.state != forge::chain::protocol::transaction_lifecycle::finalized) {
         co_return response;
      }

      if (!response.context.anchor || response.context.anchor->chain != chain || !response.receipt ||
          !response.audit || !response.audit->transaction) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_transaction_proof,
                               "chain outbox requires a verified finalized receipt");
      }
      const auto& receipt = response.receipt->trx;
      const auto* packed = std::get_if<forge::chain::protocol::packed_transaction>(&receipt);
      const auto receipt_id = packed ? packed->id() : std::get<forge::chain::protocol::transaction_id>(receipt);
      if (receipt_id != transaction) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_transaction_proof,
                               "chain outbox receipt names another transaction");
      }
      co_return response;
   }

   boost::asio::awaitable<void>
   retain_final(const detail::outbox_attempt& expected, forge::chain::protocol::transaction_status_response result) {
      auto active = co_await store.begin_transaction();
      auto current = co_await active.find(expected.id);
      if (current && current->state != detail::delivery_state::finalized && matches(*current, expected)) {
         current->state = detail::delivery_state::finalized;
         current->final = std::move(result);
         co_await active.replace(std::move(*current));
         co_await active.commit();
      }
   }

   boost::asio::awaitable<void> finish(const detail::outbox_attempt& expected, detail::delivery_state state) {
      auto active = co_await store.begin_transaction();
      auto current = co_await active.find(expected.id);
      if (current && current->pending() && current->revision == expected.revision && matches(*current, expected)) {
         current->state = state;
         current->next_check = now_ms() + static_cast<std::uint64_t>(settings.poll_interval.count());
         co_await active.replace(std::move(*current));
         co_await active.commit();
      }
   }

   boost::asio::awaitable<bool> mark_replay(detail::outbox_attempt& expected) {
      auto active = co_await store.begin_transaction();
      auto current = co_await active.find(expected.id);
      if (!current || current->revision != expected.revision || current->state != detail::delivery_state::possible ||
          !matches(*current, expected) || current->expires <= now_ms() ||
          current->sends >= settings.max_sends) {
         co_return false;
      }

      ++current->sends;
      expected.sends = current->sends;
      co_await active.replace(std::move(*current));
      co_await active.commit();
      co_return true;
   }

   boost::asio::awaitable<detail::delivery_state> send(const detail::outbox_attempt& attempt, destination target,
                                                      std::stop_token stop) {
      require_running(stop);
      if (attempt.expires <= now_ms()) {
         co_return detail::delivery_state::possible;
      }
      const auto response = co_await io.submit(
          attempt.chain, std::move(target),
          {.transaction = attempt.packed, .retry = false,
           .timeout_ms = static_cast<std::uint64_t>(settings.timeout.count())}, stop);
      if (response.id != attempt.transaction || (response.trace && response.trace->id != attempt.transaction)) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_transaction_proof,
                               "chain outbox submission acknowledgement has another transaction ID");
      }
      switch (response.state) {
      case forge::chain::protocol::transaction_lifecycle::accepted:
      case forge::chain::protocol::transaction_lifecycle::included:
      case forge::chain::protocol::transaction_lifecycle::finalized:
         co_return detail::delivery_state::acknowledged;
      case forge::chain::protocol::transaction_lifecycle::rejected:
         // A rejection during replay cannot resolve an earlier potentially delivered packet.
         co_return attempt.sends == 1 ? detail::delivery_state::rejected : detail::delivery_state::possible;
      default:
         co_return detail::delivery_state::possible;
      }
   }

   bool should_replay(const detail::outbox_attempt& attempt,
                      const std::optional<forge::chain::protocol::transaction_status_response>& observed) const {
      if (attempt.state != detail::delivery_state::possible || attempt.mode != outbox::replay::same_packed ||
          attempt.sends >= settings.max_sends || attempt.expires <= now_ms()) {
         return false;
      }
      const auto state = observed ? observed->state : forge::chain::protocol::transaction_lifecycle::unknown;
      if (state == forge::chain::protocol::transaction_lifecycle::accepted ||
          state == forge::chain::protocol::transaction_lifecycle::included) {
         return false;
      }
      if (attempt.policy == "same_packed" && attempt.policy_version == 1) {
         return true;
      }
      const auto expires = forge::chain::protocol::time_point_sec{static_cast<std::uint32_t>(attempt.expires / 1000)};
      return settings.decide &&
             settings.decide({attempt.mode, attempt.policy, attempt.policy_version},
                             {attempt.transaction, expires, attempt.sends, state}) == outbox::replay::same_packed;
   }

   boost::asio::awaitable<void> advance(forge::chain::protocol::chain_id chain,
                                        forge::chain::protocol::transaction_id transaction, std::stop_token stop) {
      require_running(stop);
      const auto key = std::pair{chain, transaction};
      {
         const auto lock = std::scoped_lock{advancing_mutex};
         if (!advancing.insert(key).second) {
            co_return;
         }
      }
      const auto release = boost::scope::scope_exit([&] {
         const auto lock = std::scoped_lock{advancing_mutex};
         advancing.erase(key);
         changed.notify();
      });
      auto active = co_await store.begin_transaction();
      auto attempt = co_await find(active, chain, transaction);
      const auto now = now_ms();
      if (!attempt || !attempt->pending() || attempt->next_check > now) {
         co_await active.rollback();
         co_return;
      }
      const auto first = attempt->state == detail::delivery_state::staged;
      const auto expired = attempt->expires <= now;
      if (first && expired) {
         attempt->state = detail::delivery_state::expired;
         co_await active.replace(std::move(*attempt));
         co_await active.commit();
         co_return;
      }

      auto target = decode_destination(attempt->peer);
      if (io.configured && !io.configured(target)) {
         if (expired) {
            attempt->state = detail::delivery_state::unresolved;
         } else {
            attempt->next_check = now + static_cast<std::uint64_t>(settings.poll_interval.count());
         }
         co_await active.replace(std::move(*attempt));
         co_await active.commit();
         co_return;
      }

      if (attempt->revision == std::numeric_limits<std::uint64_t>::max()) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::resource_exhausted, "chain outbox revision exhausted");
      }

      ++attempt->revision;
      // Hold one durable claim through the bounded I/O window, including on a lost commit reply.
      attempt->next_check = now + static_cast<std::uint64_t>(settings.timeout.count());
      if (first) {
         attempt->state = detail::delivery_state::possible;
         attempt->sends = 1;
      } else if (expired) {
         // Take one final bounded observation. Later queries can still use result().
         attempt->state = detail::delivery_state::unresolved;
      }
      co_await active.replace(*attempt);
      co_await active.commit();

      auto next = attempt->state;
      auto failure = std::exception_ptr{};
      try {
         if (first) {
            next = co_await send(*attempt, std::move(target), stop);
         } else {
            auto observation = co_await status(chain, target, transaction, stop);
            if (observation && observation->state == forge::chain::protocol::transaction_lifecycle::finalized) {
               co_await retain_final(*attempt, std::move(*observation));
               co_return;
            }
            if (should_replay(*attempt, observation) && co_await mark_replay(*attempt)) {
               next = co_await send(*attempt, std::move(target), stop);
            }
         }
      } catch (...) {
         failure = std::current_exception();
      }

      if (!stop.stop_requested()) {
         co_await finish(*attempt, next);
      }
      if (failure) {
         std::rethrow_exception(failure);
      }
   }
};

outbox::outbox(storage store, transport io, options settings)
    : impl_(std::make_unique<impl>(std::move(store), std::move(io), std::move(settings))) {
   if (!impl_->io.submit || !impl_->io.status ||
       impl_->settings.batch_limit == 0 || impl_->settings.batch_limit > service_limits.max_page_size ||
       impl_->settings.max_sends == 0 ||
       impl_->settings.poll_interval.count() <= 0 || impl_->settings.timeout.count() <= 0 ||
       static_cast<std::uint64_t>(impl_->settings.timeout.count()) > service_limits.max_await_ms) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request, "invalid chain outbox configuration");
   }
   impl_->store.register_object(forge::db::object::registration::of<detail::outbox_attempt_index>());
}

outbox::~outbox() = default;

boost::asio::awaitable<forge::chain::protocol::transaction_id>
outbox::stage(forge::db::object::transaction& active, forge::chain::protocol::chain_id chain, destination target,
              const forge::chain::protocol::packed_transaction& transaction,
              delivery_policy policy) {
   auto record = make_attempt(chain, target, transaction, std::move(policy));
   auto attached = co_await impl_->store.join(active);
   const auto id = record.transaction;
   co_await impl_->stage(attached, std::move(record), {});
   co_return id;
}

boost::asio::awaitable<forge::chain::protocol::transaction_id>
outbox::submit(forge::chain::protocol::chain_id chain, destination target,
               forge::chain::protocol::packed_transaction transaction, delivery_policy policy,
               admission admit, std::stop_token parent_stop) {
   auto record = make_attempt(chain, target, std::move(transaction), std::move(policy));
   const auto id = record.transaction;
   auto scope = forge::asio::cancellation_scope{co_await boost::asio::this_coro::executor, std::array{parent_stop},
                                                co_await boost::asio::this_coro::cancellation_state};
   const auto acknowledged = co_await scope.run<forge::chain::protocol::transaction_id>(
       [&](std::stop_token stop) -> boost::asio::awaitable<forge::chain::protocol::transaction_id> {
          require_running(stop);
          auto active = co_await impl_->store.begin_transaction();
          co_await impl_->stage(active, std::move(record), admit);
          require_running(stop);
          co_await active.commit();
          co_await impl_->advance(chain, id, stop);
          for (;;) {
             const auto observed = impl_->changed.epoch();
             const auto saved = co_await impl_->read(chain, id);
             if (saved && is_acknowledged(saved->state)) {
                co_return id;
             }
             if (saved && saved->state == detail::delivery_state::rejected) {
                FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::admission_rejected,
                                      "chain rejected the outbox transaction");
             }
             if (saved && saved->state == detail::delivery_state::expired) {
                FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::deadline_exceeded,
                                      "unsent outbox transaction expired");
             }
             if (impl_->in_flight(chain, id)) {
                co_await impl_->changed.async_wait(observed, stop);
             } else if (observed == impl_->changed.epoch()) {
                FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable,
                                      "chain outbox outcome remains pending");
             }
          }
       },
       impl_->settings.timeout);
   require_running(scope.stop_token());
   co_return acknowledged;
}

boost::asio::awaitable<std::optional<forge::chain::protocol::transaction_status_response>>
outbox::result(forge::chain::protocol::chain_id chain, destination target,
               forge::chain::protocol::transaction_id transaction, std::stop_token parent_stop) {
   const auto stored_destination = encode_destination(chain, target);
   auto scope = forge::asio::cancellation_scope{co_await boost::asio::this_coro::executor, std::array{parent_stop},
                                                co_await boost::asio::this_coro::cancellation_state};
   auto final = std::optional<forge::chain::protocol::transaction_status_response>{};
   co_await scope.run<void>(
       [&](std::stop_token stop) -> boost::asio::awaitable<void> {
          require_running(stop);
          auto saved = co_await impl_->read(chain, transaction);
          if (saved && saved->peer != stored_destination) {
             FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::conflict,
                                   "chain outbox transaction has another destination");
          }
          if (saved && saved->final) {
             final = std::move(saved->final);
             co_return;
          }

          auto observed = co_await impl_->status(chain, std::move(target), transaction, stop);
          if (observed && observed->state == forge::chain::protocol::transaction_lifecycle::finalized) {
             if (saved) {
                co_await impl_->retain_final(*saved, *observed);
             }
             final = std::move(observed);
          }
       },
       impl_->settings.timeout);
   require_running(scope.stop_token());
   co_return final;
}

boost::asio::awaitable<void> outbox::recover(std::stop_token parent_stop) {
   auto scope = forge::asio::cancellation_scope{co_await boost::asio::this_coro::executor, std::array{parent_stop},
                                                co_await boost::asio::this_coro::cancellation_state};
   co_await scope.run<void>(
       [&](std::stop_token stop) -> boost::asio::awaitable<void> {
          require_running(stop);
          auto read = co_await impl_->store.begin_read();
          auto due = co_await read.index<detail::outbox_attempt_index, detail::by_due>()
              .range(std::tuple{true, std::uint64_t{0}}, std::tuple{true, now_ms() + 1})
              .page({.limit = static_cast<std::uint32_t>(impl_->settings.batch_limit)});
          read = {};
          for (const auto& attempt : due.items) {
             if (stop.stop_requested()) {
                co_return;
             }
             try {
                co_await impl_->advance(attempt.chain, attempt.transaction, stop);
             } catch (const std::exception&) {
                // The claim schedules another reconciliation even if I/O or its acknowledgement fails.
             }
          }
       },
       impl_->settings.timeout);
}

} // namespace forge::api::chain
