module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

module forge.db.object.store;

import forge.asio.gate;
import forge.db.core.exceptions;
import forge.db.ids.object_id;
import forge.db.object.exceptions;
import forge.db.object.header;
import forge.raw.raw;

#include "details/store_impl.hxx"
#include "details/record_key.hxx"

namespace forge::db::object {

runtime_state::runtime_state()
    : write_gate{std::make_shared<forge::asio::gate>()}, allocator_gate{std::make_shared<forge::asio::gate>()} {}

namespace {

constexpr auto sequence_value_size = std::size_t{8};
constexpr auto header_value_size = sizeof(std::uint64_t) + sizeof(std::uint32_t);

std::vector<std::byte> encode_header(const forge::db::object::header& value) {
   const auto packed = forge::raw::pack(value);
   auto bytes = std::vector<std::byte>{};
   bytes.reserve(packed.size());
   for (const auto byte : packed) {
      bytes.push_back(static_cast<std::byte>(byte));
   }
   return bytes;
}

forge::db::object::header decode_header(const std::vector<std::byte>& bytes) {
   if (bytes.size() != header_value_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_header, "db object header has invalid size",
                            forge::exceptions::ctx("size", bytes.size()),
                            forge::exceptions::ctx("expected-size", header_value_size));
   }

   auto packed = forge::raw::bytes{};
   packed.reserve(bytes.size());
   for (const auto byte : bytes) {
      packed.push_back(std::to_integer<std::uint8_t>(byte));
   }

   try {
      return forge::raw::unpack<forge::db::object::header>(packed);
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_header, "db object header cannot be decoded",
                            forge::exceptions::ctx("error", error.what()));
   }
}

std::vector<std::byte> encode_next_instance(std::uint64_t value) {
   auto out = std::vector<std::byte>{};
   out.reserve(sequence_value_size);
   detail::record_key::append_be64(out, value);
   return out;
}

std::uint64_t decode_next_instance(const std::vector<std::byte>& bytes) {
   if (bytes.size() != sequence_value_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object id sequence record has invalid size");
   }

   auto value = std::uint64_t{0};
   for (auto byte : bytes) {
      value <<= 8U;
      value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(byte));
   }
   return value;
}

struct runtime_entry {
   std::weak_ptr<forge::db::core::driver> driver;
   std::string family;
   std::weak_ptr<runtime_state> runtime;
};

std::vector<runtime_entry>& runtime_registry() {
   static auto entries = std::vector<runtime_entry>{};
   return entries;
}

std::mutex& runtime_registry_mutex() {
   static auto mutex = std::mutex{};
   return mutex;
}

std::shared_ptr<runtime_state> acquire_runtime_state(const std::shared_ptr<forge::db::core::driver>& driver,
                                                     const forge::db::core::family& family) {
   auto guard = std::scoped_lock{runtime_registry_mutex()};
   auto& entries = runtime_registry();

   entries.erase(
       std::remove_if(entries.begin(), entries.end(),
                      [](const runtime_entry& entry) { return entry.driver.expired() || entry.runtime.expired(); }),
       entries.end());

   for (const auto& entry : entries) {
      const auto less = std::owner_less<void>{};
      if (!entry.driver.expired() && entry.family == family.name &&
          !less(entry.driver, driver) && !less(driver, entry.driver)) {
         if (auto runtime = entry.runtime.lock()) {
            return runtime;
         }
      }
   }

   auto runtime = std::make_shared<runtime_state>();
   entries.push_back(runtime_entry{.driver = driver, .family = family.name, .runtime = runtime});
   return runtime;
}

} // namespace

store::impl::impl(std::shared_ptr<forge::db::core::driver> driver_value, store::config config_value,
                  store::options options_value)
    : driver{std::move(driver_value)}, config{std::move(config_value)}, settings{options_value} {
   if (!driver) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object driver is null");
   }
   if (config.family.name.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object family is empty");
   }
   runtime = acquire_runtime_state(driver, config.family);
   registered.emplace(object_id_of<header_index>::value, std::type_index{typeid(header_index)});
}

boost::asio::awaitable<forge::db::core::transaction> store::impl::open_write_transaction() const {
   if (!driver) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object driver is null");
   }
   co_return co_await driver->begin_transaction();
}

boost::asio::awaitable<forge::db::core::snapshot> store::impl::open_read_snapshot() const {
   if (!driver) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object driver is null");
   }
   co_return co_await driver->begin_read();
}

boost::asio::awaitable<forge::db::object::header>
store::impl::initialize_header(forge::db::core::transaction& active) const {
   const auto key = detail::record_key::object(forge::db::ids::to_object_id(forge::db::object::header_id));
   const auto existing = co_await active.get(config.family, key);

   if (!existing.has_value()) {
      const auto records = co_await active.scan_page(config.family, forge::db::core::record_range{.has_end = false},
                                                     forge::db::core::page_request{.limit = 1});
      if (!records.entries.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::incompatible_version, "db object family is non-empty but has no header",
                               forge::exceptions::ctx("family", config.family.name));
      }

      auto created = forge::db::object::header{};
      created.id = forge::db::object::header_id;
      co_await active.put(config.family, key, encode_header(created));
      co_return created;
   }

   auto decoded = decode_header(*existing);
   if (decoded.id != forge::db::object::header_id) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_header, "db object header has invalid id");
   }
   if (decoded.version < forge::db::object::header::minimum_version ||
       decoded.version > forge::db::object::header::current_version) {
      FORGE_THROW_EXCEPTION(exceptions::incompatible_version, "db object header version is incompatible",
                            forge::exceptions::ctx("version", decoded.version),
                            forge::exceptions::ctx("minimum-version", forge::db::object::header::minimum_version),
                            forge::exceptions::ctx("current-version", forge::db::object::header::current_version));
   }

   if (decoded.version != forge::db::object::header::current_version) {
      decoded.version = forge::db::object::header::current_version;
      co_await active.put(config.family, key, encode_header(decoded));
   }
   co_return decoded;
}

boost::asio::awaitable<forge::db::ids::object_id> store::impl::allocate_id(forge::db::ids::object_id type,
                                                                           forge::db::core::transaction& active) {
   const auto key = detail::record_key::sequence(type);
   if (settings.id_allocation == id_allocation_policy::transactional) {
      const auto existing = co_await active.get(config.family, key);
      auto next = existing.has_value() ? decode_next_instance(*existing) : std::uint64_t{0};
      auto candidate = type;
      candidate.instance = next;
      while ((co_await active.get(config.family, detail::record_key::object(candidate))).has_value()) {
         if (next == std::numeric_limits<std::uint64_t>::max()) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object id sequence is exhausted");
         }
         ++next;
         candidate.instance = next;
      }
      if (next == std::numeric_limits<std::uint64_t>::max()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object id sequence is exhausted");
      }

      auto allocated = type;
      allocated.instance = next;
      co_await active.put(config.family, key, encode_next_instance(next + 1U));
      co_return allocated;
   }

   const auto ticket = co_await runtime->allocator_gate->acquire();
   auto allocated = type;

   auto cursor = runtime->next_instances.find(type);
   if (cursor == runtime->next_instances.end()) {
      const auto existing = co_await active.get(config.family, key);
      const auto next = existing.has_value() ? decode_next_instance(*existing) : std::uint64_t{0};
      cursor = runtime->next_instances.emplace(type, next).first;
   }

   auto next = cursor->second;
   if (next == std::numeric_limits<std::uint64_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object id sequence is exhausted");
   }

   auto candidate = type;
   candidate.instance = next;
   while ((co_await active.get(config.family, detail::record_key::object(candidate))).has_value()) {
      if (next == std::numeric_limits<std::uint64_t>::max()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object id sequence is exhausted");
      }
      ++next;
      candidate.instance = next;
   }
   if (next == std::numeric_limits<std::uint64_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object id sequence is exhausted");
   }

   allocated.instance = next;
   cursor->second = next + 1U;
   co_await active.put(config.family, key, encode_next_instance(cursor->second));
   co_return allocated;
}

boost::asio::awaitable<void> store::impl::seal_allocations(transaction::allocation_seal_map seals) {
   if (seals.empty()) {
      co_return;
   }

   const auto ticket = co_await runtime->allocator_gate->acquire();
   auto active = co_await open_write_transaction();
   auto error = std::exception_ptr{};

   try {
      for (auto& [type, consumed_next] : seals) {
         auto sealed_type = type;
         sealed_type.instance = 0;
         const auto key = detail::record_key::sequence(sealed_type);
         const auto existing = co_await active.get(config.family, key);
         const auto stored_next = existing.has_value() ? decode_next_instance(*existing) : std::uint64_t{0};
         const auto sealed_next = std::max(stored_next, consumed_next);
         if (sealed_next != stored_next) {
            co_await active.put(config.family, key, encode_next_instance(sealed_next));
         }

         auto cursor = runtime->next_instances.find(sealed_type);
         if (cursor == runtime->next_instances.end()) {
            runtime->next_instances.emplace(sealed_type, sealed_next);
         } else {
            cursor->second = std::max(cursor->second, sealed_next);
         }
      }

      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }

   if (error) {
      try {
         co_await active.rollback();
      } catch (...) {
      }
      std::rethrow_exception(error);
   }
   co_return;
}

void store::impl::register_object_type(forge::db::ids::object_id type, std::type_index model) {
   if (registered.contains(type)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object type is already registered");
   }
   registered.emplace(type, model);
}

void store::impl::ensure_registered_type(forge::db::ids::object_id type, std::type_index model) const {
   const auto found = registered.find(type);
   if (found == registered.end() || found->second != model) {
      FORGE_THROW_EXCEPTION(exceptions::unregistered_object, "db object type is not registered");
   }
}

} // namespace forge::db::object
