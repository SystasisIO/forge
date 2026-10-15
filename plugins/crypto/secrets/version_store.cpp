module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/describe.hpp>
#include <forge/exceptions/macros.hpp>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.crypto.secrets.plugin;

import forge.app.plugin_lifecycle;
import forge.asio.compute;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.keystore.ownership;
import forge.crypto.keystore.types;
import forge.api.core.exceptions;
import forge.crypto.core.random;
import forge.crypto.keystore.encrypted_file;
import forge.crypto.keystore.password;
import forge.crypto.keystore.persistence;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.version_types;

#include "details/version_store.hxx"
#include "details/version_payload.hxx"
#include "details/version_store_owned.hxx"

namespace forge::plugins::crypto::secrets {

namespace {

void cleanup_temporaries(const std::filesystem::path& path) {
   auto entries = std::uint64_t{};
   auto name_bytes = std::uint64_t{};
   auto removed = std::uint64_t{};
   const auto prefix = path.filename().string() + ".tmp-";
   for (const auto& entry : std::filesystem::directory_iterator{path.parent_path()}) {
      const auto name = entry.path().filename().string();
      if (++entries > 128 || name.size() > 32U * 1024U - name_bytes) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_store, "Managed secret directory requires cleanup");
      }
      name_bytes += name.size();
      if (!name.starts_with(prefix)) {
         continue;
      }
      const auto suffix = std::string_view{name}.substr(prefix.size());
      struct stat status{};
      if (suffix.size() != 24 ||
          !std::ranges::all_of(
              suffix,
              [](unsigned char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }) ||
          ::lstat(entry.path().c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
          (status.st_mode & 0077U) != 0 || status.st_nlink != 1 || ++removed > 32 ||
          ::unlink(entry.path().c_str()) != 0) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_store, "Managed secret temporary cleanup failed");
      }
   }
}

bool is_below(const std::filesystem::path& path, const std::filesystem::path& directory) {
   auto entry = path.begin();
   for (const auto& part : directory) {
      if (entry == path.end() || *entry++ != part) {
         return false;
      }
   }
   return true;
}

} // namespace

version_store::version_store(version_store_config settings, forge::crypto::keystore::decrypt_limits limits,
                             forge::app::plugin_lifecycle lifecycle)
    : _settings{std::move(settings)}, _limits{[&] {
         limits.max_plaintext_bytes = _settings.max_store_plaintext_bytes;
         return limits;
      }()},
      _lifecycle{std::move(lifecycle)}, _process{static_cast<std::uint64_t>(::getpid())} {
   validate_config();
}

version_store::~version_store() = default;

boost::asio::awaitable<void> version_store::initialize(forge::asio::compute::executor compute) {
   check_process();
   return isolated(initialize_owned(shared_from_this(), std::move(compute)));
}

boost::asio::awaitable<void> version_store::initialize_owned(std::shared_ptr<version_store> owner,
                                                             forge::asio::compute::executor compute) {
   owner->check_process();
   if (!owner->_settings.enabled) {
      co_return;
   }
   if (!compute.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Managed secrets require the application's compute executor");
   }
   {
      auto lock = std::scoped_lock{owner->_mutex};
      if (owner->_stopping) {
         FORGE_THROW_EXCEPTION(exceptions::stopped, "Managed secret admission is closed");
      }
      owner->_compute = std::move(compute);
   }
   static_cast<void>(co_await owner->blocking([owner] {
      try {
         owner->open();
      } catch (...) {
         owner->fence();
         throw;
      }
      return true;
   }));
}

void version_store::open() {
   const auto path = std::filesystem::path{_settings.path};
   try {
      _password = forge::crypto::keystore::read_password({
          .source = forge::crypto::keystore::password_source::file,
          .file = _settings.bootstrap_passphrase_file,
          .max_bytes = 64U * 1024U,
      });
      auto directory = std::filesystem::weakly_canonical(path.parent_path());
      auto bootstrap = std::filesystem::canonical(_settings.bootstrap_passphrase_file);
      if (is_below(bootstrap, directory)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_source, "Managed bootstrap must be outside the store directory");
      }
   } catch (const exceptions::invalid_source&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_source, "Managed secret bootstrap is unavailable");
   }
   try {
      if (_settings.mode == version_open_mode::create_new) {
         forge::crypto::keystore::prepare_private_directory(path.parent_path());
      }
      _owner = forge::crypto::keystore::ownership::acquire(path);
      _access = _owner->claim();
      cleanup_temporaries(_owner->path());
      if (_settings.mode == version_open_mode::create_new) {
         auto candidate = std::make_shared<version_catalog>();
         candidate->receipt_key =
             std::make_shared<forge::crypto::core::secret_bytes>(forge::crypto::core::random_bytes(32));
         persist(std::move(candidate), false);
      } else {
         _owner->require_file(_settings.max_store_plaintext_bytes + 4096);
         auto container = forge::crypto::keystore::read_container(_owner, _settings.max_store_plaintext_bytes + 4096);
         auto plaintext = forge::crypto::keystore::decrypt_file(container, _password, _limits);
         auto candidate = version_payload::decode(plaintext, _settings);
         validate_catalog(*candidate);
         publish(std::move(candidate), false);
      }
   } catch (const exceptions::invalid_config&) {
      throw;
   } catch (const exceptions::durability_unknown&) {
      throw;
   } catch (const exceptions::invalid_store&) {
      throw;
   } catch (const forge::crypto::keystore::exceptions::in_use&) {
      FORGE_THROW_EXCEPTION(exceptions::store_unavailable, "Managed secret store is already owned");
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "Managed secret store cannot be opened");
   }
}

version_store::reservation::reservation(std::shared_ptr<version_store> owner, std::uint64_t bytes, bool crypto)
    : _owner{std::move(owner)}, _bytes{bytes}, _crypto{crypto} {}

version_store::reservation::~reservation() {
   if (_active) {
      _owner->release(_bytes, _crypto);
   }
}

void version_store::reservation::activate() noexcept {
   _active = true;
}

std::unique_ptr<version_store::reservation> version_store::reserve(std::uint64_t bytes, bool crypto) {
   if (_process != static_cast<std::uint64_t>(::getpid())) {
      FORGE_THROW_EXCEPTION(exceptions::ownership_lost, "Managed secret ownership is no longer valid");
   }
   const auto phase = _lifecycle.snapshot();
   if (phase.stop_requested) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "Managed secret admission is closed");
   }
   if (!_settings.enabled) {
      FORGE_THROW_EXCEPTION(exceptions::store_unavailable, "Managed secret store is disabled");
   }
   if (phase.phase == forge::app::lifecycle_phase::created || phase.phase == forge::app::lifecycle_phase::configured) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "Managed secret admission is closed");
   }
   auto ticket = std::make_unique<reservation>(shared_from_this(), bytes, crypto);
   if (!_lifecycle.try_admit(phase.generation, [&] {
          auto lock = std::scoped_lock{_mutex};
          if (_stopping) {
             FORGE_THROW_EXCEPTION(exceptions::stopped, "Managed secret admission is closed");
          }
          if (!_ready || _fault) {
             FORGE_THROW_EXCEPTION(exceptions::store_unavailable, "Managed secret store is unavailable");
          }
          if (_pending >= _settings.max_pending_calls || bytes > _settings.max_pending_bytes - _pending_bytes ||
              (crypto && _crypto_calls >= 2)) {
             FORGE_THROW_EXCEPTION(exceptions::busy, "Managed secret operation capacity is exhausted");
          }
          ++_pending;
          _pending_bytes += bytes;
          if (crypto) {
             ++_crypto_calls;
          }
          ticket->activate();
       })) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "Managed secret admission is closed");
   }
   return ticket;
}

void version_store::release(std::uint64_t bytes, bool crypto) noexcept {
   {
      auto lock = std::scoped_lock{_mutex};
      --_pending;
      _pending_bytes -= bytes;
      if (crypto) {
         --_crypto_calls;
      }
   }
   _drained.notify();
}

void version_store::request_stop() noexcept {
   if (_process != static_cast<std::uint64_t>(::getpid())) {
      return;
   }
   auto lock = std::scoped_lock{_mutex};
   _stopping = true;
}

boost::asio::awaitable<void> version_store::shutdown() {
   check_process();
   return isolated(shutdown_owned(shared_from_this()), false);
}

boost::asio::awaitable<void> version_store::shutdown_owned(std::shared_ptr<version_store> owner) {
   owner->check_process();
   owner->request_stop();
   for (;;) {
      const auto observed = owner->_drained.epoch();
      {
         auto lock = std::scoped_lock{owner->_mutex};
         if (owner->_pending == 0) {
            break;
         }
      }
      co_await owner->_drained.async_wait(observed);
   }
   owner->_write_gate.close();
   if (owner->_compute.valid()) {
      static_cast<void>(co_await owner->blocking([owner] {
         owner->clear();
         return true;
      }));
   }
}

void version_store::clear() noexcept {
   auto retired = std::shared_ptr<const version_catalog>{};
   {
      auto lock = std::scoped_lock{_mutex};
      retired = std::move(_catalog);
      _ready = false;
   }
   retired.reset();
   _password.clear();
   _access.reset();
   _owner.reset();
}

version_snapshot version_store::status() const {
   check_process();
   auto lock = std::scoped_lock{_mutex};
   return {
       .enabled = _settings.enabled,
       .ready = _ready && !_stopping && !_fault,
       .stopping = _stopping,
       .fault = _fault,
       .versions = _catalog ? static_cast<std::uint64_t>(_catalog->versions.size()) : 0,
       .receipts = _catalog ? static_cast<std::uint64_t>(_catalog->receipts.size()) : 0,
   };
}

boost::asio::awaitable<version_snapshot> version_store::status_async() {
   return status_owned(shared_from_this());
}

boost::asio::awaitable<version_snapshot> version_store::status_owned(std::shared_ptr<version_store> owner) {
   co_return owner->status();
}

void version_store::publish(std::shared_ptr<const version_catalog> candidate, bool fault) noexcept {
   auto retired = std::shared_ptr<const version_catalog>{};
   {
      auto lock = std::scoped_lock{_mutex};
      retired = std::exchange(_catalog, std::move(candidate));
      _fault = fault;
      _ready = !fault;
   }
}

void version_store::fence() noexcept {
   auto lock = std::scoped_lock{_mutex};
   _fault = true;
   _ready = false;
}

void version_store::persist(std::shared_ptr<version_catalog> candidate, bool replace) {
   auto plaintext = version_payload::encode(*candidate, _settings.max_store_plaintext_bytes);
   candidate->encoded_bytes = plaintext.size();
   require_capacity(*candidate, plaintext.size());
   auto container = forge::crypto::keystore::encrypt_file({
       .plaintext = std::move(plaintext),
       .password = _password,
   });
   try {
      forge::crypto::keystore::write_container(_owner, container, replace);
   } catch (const forge::crypto::keystore::exceptions::durability_unknown&) {
      publish(std::move(candidate), true);
      FORGE_THROW_EXCEPTION(exceptions::durability_unknown, "Managed secret durability is unknown");
   } catch (const forge::crypto::keystore::exceptions::ownership_lost&) {
      fence();
      FORGE_THROW_EXCEPTION(exceptions::ownership_lost, "Managed secret ownership is no longer valid");
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::store_unavailable, "Managed secret update failed");
   }
   publish(std::move(candidate), false);
}

std::shared_ptr<const version_catalog> version_store::current() {
   try {
      _owner->verify();
   } catch (const std::exception&) {
      fence();
      FORGE_THROW_EXCEPTION(exceptions::ownership_lost, "Managed secret ownership is no longer valid");
   }
   auto lock = std::scoped_lock{_mutex};
   if (_fault || !_catalog) {
      FORGE_THROW_EXCEPTION(exceptions::store_unavailable, "Managed secret store is unavailable");
   }
   return _catalog;
}

} // namespace forge::plugins::crypto::secrets
