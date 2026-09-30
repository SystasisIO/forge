#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import forge.api.auth.authenticated_caller;
import forge.api.core.registry;
import forge.api.core.trusted_invocation;
import forge.api.http.openapi;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.asio.compute;
import forge.crypto.keystore.store;
import forge.crypto.wallet.api;
import forge.plugins.crypto.wallet.plugin;
import forge.schema.object;
import forge.variant.value;

namespace wallet = forge::crypto::wallet;
namespace plugin = forge::plugins::crypto::wallet;
namespace keystore = forge::crypto::keystore;
namespace asymmetric = forge::crypto::asymmetric;
using forge::asio::blocking::run;

namespace {

struct temporary_directory {
   temporary_directory() {
      auto pattern = (std::filesystem::temp_directory_path() / "forge-wallet-XXXXXX").string();
      const auto* created = ::mkdtemp(pattern.data());
      BOOST_REQUIRE(created != nullptr);
      path = created;
   }
   ~temporary_directory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
   }
   std::filesystem::path path;
};

struct provider_fixture {
   provider_fixture() : scheduler{runtime}, value{"producer"} {
      value.initialize(directory.path, scheduler, compute.get_executor());
      value.startup();
   }
   ~provider_fixture() {
      run(runtime, value.shutdown());
   }
   temporary_directory directory;
   forge::asio::runtime runtime{{.worker_threads = 2}};
   forge::asio::task::scheduler scheduler;
   forge::asio::compute::pool compute{{.worker_threads = 2}};
   plugin::provider value;
};

struct blocking_pool_guard {
   explicit blocking_pool_guard(forge::asio::runtime& runtime, forge::asio::compute::executor compute)
       : runtime{runtime} {
      auto released = release.get_future().share();
      auto started = entered.get_future();
      task = run(runtime, compute.submit({.name = "wallet.test.block"}, [this, released] {
         entered.set_value();
         released.wait();
      }));
      BOOST_REQUIRE(started.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   }
   ~blocking_pool_guard() {
      finish();
   }
   void finish() {
      if (!done) {
         done = true;
         release.set_value();
         run(runtime, std::move(task).wait());
      }
   }
   forge::asio::runtime& runtime;
   std::promise<void> release;
   std::promise<void> entered;
   forge::asio::compute::operation<void> task;
   bool done = false;
};

void flush(forge::asio::runtime& runtime) {
   run(runtime, boost::asio::post(runtime.context(), boost::asio::use_awaitable));
}

forge::api::auth::authenticated_caller
caller(bool trusted = true, forge::api::auth::caller_source source = forge::api::auth::caller_source::tls_certificate) {
   auto value = forge::api::auth::authenticated_caller{
       source, forge::crypto::digest::sha256::hash(std::string{"wallet-test-client"})};
   if (trusted) {
      auto invocation = forge::api::core::trusted_invocation_builder{}.set(value).build();
      BOOST_REQUIRE(
          forge::api::core::server_supplied<forge::api::auth::authenticated_caller>::apply(value, invocation));
   }
   return value;
}

struct plugin_fixture {
   explicit plugin_fixture(plugin::config settings,
                           std::function<void(plugin::config&, const std::filesystem::path&)> prepare = {})
       : scheduler{runtime} {
      settings.directory = directory.path.string();
      if (prepare) {
         prepare(settings, directory.path);
      }
      value = std::make_unique<plugin::plugin>(plugin::plugin_options{.initial_config = std::move(settings)});
      auto installer = forge::api::core::installer{apis};
      run(runtime, value->provide(installer));
      auto context = forge::app::plugin_context{scheduler, apis, signals, events, nullptr, {}, compute.get_executor()};
      run(runtime, value->initialize(context));
      run(runtime, value->startup());
   }
   ~plugin_fixture() {
      if (value) {
         run(runtime, value->shutdown());
      }
   }
   auto api() {
      return apis.get<wallet::api>(wallet::api::ref());
   }
   temporary_directory directory;
   forge::asio::runtime runtime{{.worker_threads = 2}};
   forge::asio::task::scheduler scheduler;
   forge::api::core::registry apis;
   forge::app::signal_bus signals;
   forge::app::event_bus events;
   forge::asio::compute::pool compute{{.worker_threads = 2}};
   std::unique_ptr<plugin::plugin> value;
};

plugin::config permissions(std::vector<std::string> wallets, std::vector<wallet::operation> operations) {
   return {.permissions = {{.fingerprint = caller().fingerprint.str(),
                            .wallets = std::move(wallets),
                            .operations = std::move(operations)}}};
}

} // namespace

BOOST_AUTO_TEST_SUITE(crypto_wallet_tests)

BOOST_AUTO_TEST_CASE(wallet_openapi_excludes_caller_and_marks_secrets_in_dto_schema) {
   const auto document = forge::api::http::openapi<wallet::api>();
   BOOST_TEST(document["paths"].get_object().size() == 12U);
   const auto& create = document["paths"]["/v1/wallet/create"]["post"];
   const auto& fields = create["requestBody"]["content"]["application/json"]["schema"]["properties"];
   BOOST_TEST(fields.get_object().contains("wallet"));
   BOOST_TEST(fields.get_object().contains("password"));
   BOOST_TEST(!fields.get_object().contains("caller"));
   const auto& list = document["paths"]["/v1/wallet/list"]["post"];
   BOOST_TEST(!list.get_object().contains("requestBody"));
   BOOST_TEST(!list.get_object().contains("parameters"));
   const auto password_schema = forge::schema::rules<wallet::password_request>::define();
   const auto import_schema = forge::schema::rules<wallet::import_request>::define();
   bool password_secret = false;
   bool key_secret = false;
   for (const auto& field : password_schema.fields()) {
      if (field.name == "password") {
         password_secret = field.secret;
      }
   }
   for (const auto& field : import_schema.fields()) {
      if (field.name == "private_key") {
         key_secret = field.secret;
      }
   }
   BOOST_TEST(password_secret);
   BOOST_TEST(key_secret);
}

BOOST_AUTO_TEST_CASE(lifecycle_is_locked_by_default_and_provider_survives_lock_unlock) {
   provider_fixture f;
   BOOST_CHECK(run(f.runtime, f.value.create("test-password")).status == wallet::state::locked);
   BOOST_CHECK_THROW(run(f.runtime, f.value.keys()), wallet::exceptions::locked);
   BOOST_CHECK_THROW(run(f.runtime, f.value.unlock("wrong")), keystore::exceptions::invalid_file);
   BOOST_CHECK(run(f.runtime, f.value.status()).status == wallet::state::locked);
   BOOST_CHECK_THROW(static_cast<void>(keystore::store::open(f.directory.path / "producer.fks", "test-password")),
                     keystore::exceptions::in_use);
   BOOST_CHECK(run(f.runtime, f.value.unlock("test-password")).status == wallet::state::unlocked);
   auto created = run(f.runtime, f.value.create_key("block"));
   BOOST_TEST(created.id == "block");
   auto info = run(f.runtime, f.value.describe({.value = "block"}));
   const auto digest = forge::crypto::digest::sha256::hash(std::string{"block"});
   auto signed_value = run(f.runtime, f.value.sign_digest({.id = {.value = "block"}, .digest = digest}));
   BOOST_CHECK(asymmetric::recover(signed_value.signature, digest) == info.public_key);
   BOOST_CHECK(run(f.runtime, f.value.lock()).status == wallet::state::locked);
   BOOST_CHECK_THROW(run(f.runtime, f.value.sign_digest({.id = {.value = "block"}, .digest = digest})),
                     wallet::exceptions::locked);
   BOOST_CHECK_THROW(static_cast<void>(keystore::store::open(f.directory.path / "producer.fks", "test-password")),
                     keystore::exceptions::in_use);
   run(f.runtime, f.value.unlock("test-password"));
   BOOST_TEST(run(f.runtime, f.value.keys()).size() == 1U);
   run(f.runtime, f.value.remove_key("block"));
   BOOST_TEST(run(f.runtime, f.value.keys()).empty());
   run(f.runtime, f.value.open());
   BOOST_CHECK(run(f.runtime, f.value.status()).status == wallet::state::locked);
}

BOOST_AUTO_TEST_CASE(restart_opens_without_decrypting_and_remains_locked) {
   temporary_directory directory;
   forge::asio::runtime runtime;
   forge::asio::task::scheduler scheduler{runtime};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   {
      plugin::provider first{"producer"};
      first.initialize(directory.path, scheduler, compute.get_executor());
      first.startup();
      run(runtime, first.create("test-password"));
      run(runtime, first.unlock("test-password"));
      run(runtime, first.create_key("key"));
      run(runtime, first.shutdown());
   }
   plugin::provider second{"producer"};
   second.initialize(directory.path, scheduler, compute.get_executor());
   second.startup();
   BOOST_CHECK(run(runtime, second.open()).status == wallet::state::locked);
   BOOST_CHECK_THROW(run(runtime, second.keys()), wallet::exceptions::locked);
   run(runtime, second.unlock("test-password"));
   BOOST_TEST(run(runtime, second.keys()).size() == 1U);
   run(runtime, second.shutdown());
}

BOOST_AUTO_TEST_CASE(idle_timeout_refreshes_only_successful_signing) {
   provider_fixture f;
   run(f.runtime, f.value.create("test-password"));
   run(f.runtime, f.value.unlock("test-password"));
   run(f.runtime, f.value.create_key("key"));
   run(f.runtime, f.value.set_timeout(1));
   std::this_thread::sleep_for(std::chrono::milliseconds{650});
   run(f.runtime, f.value.sign_digest({.id = {.value = "key"}}));
   std::this_thread::sleep_for(std::chrono::milliseconds{650});
   BOOST_CHECK(run(f.runtime, f.value.status()).status == wallet::state::unlocked);
   BOOST_CHECK_THROW(run(f.runtime, f.value.sign_digest({.id = {.value = "missing"}})),
                     forge::crypto::signer::exceptions::unknown_key);
   std::this_thread::sleep_for(std::chrono::milliseconds{600});
   BOOST_CHECK(run(f.runtime, f.value.status()).status == wallet::state::locked);
}

BOOST_AUTO_TEST_CASE(management_requires_trusted_tls_identity_and_exact_operation_wallet_permission) {
   plugin_fixture f{
       permissions({"alice"}, {wallet::operation::create, wallet::operation::status, wallet::operation::unlock,
                               wallet::operation::list, wallet::operation::lock_all})};
   auto api = f.api();
   BOOST_CHECK_THROW(run(f.runtime, api->create({"alice", "secret"}, {})), wallet::exceptions::permission_denied);
   BOOST_CHECK_THROW(run(f.runtime, api->create({"alice", "secret"}, caller(false))),
                     wallet::exceptions::permission_denied);
   BOOST_CHECK_THROW(
       run(f.runtime, api->create({"alice", "secret"}, caller(true, forge::api::auth::caller_source::p2p_peer))),
       wallet::exceptions::permission_denied);
   BOOST_CHECK_THROW(run(f.runtime, api->create({"bob", "secret"}, caller())), wallet::exceptions::permission_denied);
   BOOST_CHECK(run(f.runtime, api->create({"alice", "secret"}, caller())).status == wallet::state::locked);
   BOOST_TEST(run(f.runtime, api->list(caller())).size() == 1U);
   BOOST_CHECK_THROW(run(f.runtime, api->import_key({"alice", "key", "sensitive-invalid-key"}, caller())),
                     wallet::exceptions::permission_denied);
   BOOST_CHECK_THROW(run(f.runtime, api->unlock({"alice", "wrong-sensitive-password"}, caller())),
                     wallet::exceptions::invalid_password);
   run(f.runtime, api->unlock({"alice", "secret"}, caller()));
   BOOST_TEST(run(f.runtime, api->lock_all(caller())).size() == 1U);
   BOOST_CHECK(run(f.runtime, api->status({"alice"}, caller())).status == wallet::state::locked);
}

BOOST_AUTO_TEST_CASE(successful_key_mutations_refresh_idle_but_identity_queries_do_not) {
   provider_fixture f;
   run(f.runtime, f.value.create("secret"));
   run(f.runtime, f.value.unlock("secret"));
   run(f.runtime, f.value.set_timeout(1));
   std::this_thread::sleep_for(std::chrono::milliseconds{650});
   run(f.runtime, f.value.create_key("first"));
   std::this_thread::sleep_for(std::chrono::milliseconds{650});
   BOOST_CHECK(run(f.runtime, f.value.status()).status == wallet::state::unlocked);
   const auto key = asymmetric::private_key::generate();
   run(f.runtime, f.value.import_key("second", asymmetric::encoding::forge().format(key)));
   std::this_thread::sleep_for(std::chrono::milliseconds{650});
   run(f.runtime, f.value.remove_key("second"));
   std::this_thread::sleep_for(std::chrono::milliseconds{650});
   BOOST_CHECK(run(f.runtime, f.value.status()).status == wallet::state::unlocked);
   run(f.runtime, f.value.describe({.value = "first"}));
   run(f.runtime, f.value.keys());
   std::this_thread::sleep_for(std::chrono::milliseconds{600});
   BOOST_CHECK(run(f.runtime, f.value.status()).status == wallet::state::locked);
}

BOOST_AUTO_TEST_CASE(lost_ownership_faults_wallet_and_unlock_cannot_clear_fault) {
   provider_fixture f;
   run(f.runtime, f.value.create("secret"));
   run(f.runtime, f.value.unlock("secret"));
   run(f.runtime, f.value.create_key("key"));
   const auto lock = f.directory.path / "producer.fks.lock";
   std::filesystem::rename(lock, f.directory.path / "replaced.lock");
   BOOST_CHECK_THROW(run(f.runtime, f.value.keys()), keystore::exceptions::ownership_lost);
   BOOST_CHECK(run(f.runtime, f.value.status()).status == wallet::state::fault);
   BOOST_CHECK_THROW(run(f.runtime, f.value.unlock("secret")), wallet::exceptions::storage_error);
   BOOST_CHECK(run(f.runtime, f.value.lock()).status == wallet::state::fault);
}

BOOST_AUTO_TEST_CASE(lock_and_open_finish_revocation_even_when_waiting_request_is_canceled) {
   for (bool opening : {false, true}) {
      temporary_directory directory;
      forge::asio::runtime runtime;
      forge::asio::task::scheduler scheduler{runtime, {.max_blocking_tasks = 1}};
      forge::asio::compute::pool compute{{.worker_threads = 1}};
      plugin::provider value{"producer"};
      value.initialize(directory.path, scheduler, compute.get_executor(), 0);
      value.startup();
      run(runtime, value.create("secret"));
      run(runtime, value.unlock("secret"));
      run(runtime, value.create_key("key"));
      blocking_pool_guard busy{runtime, compute.get_executor()};
      auto signing = boost::asio::co_spawn(runtime.context(), value.sign_digest({.id = {.value = "key"}}),
                                           boost::asio::use_future);
      flush(runtime);
      auto cancellation = boost::asio::cancellation_signal{};
      auto locking =
          boost::asio::co_spawn(runtime.context(), opening ? value.open() : value.lock(),
                                boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
      flush(runtime);
      cancellation.emit(boost::asio::cancellation_type::all);
      flush(runtime);
      BOOST_CHECK(locking.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
      busy.finish();
      BOOST_CHECK_NO_THROW(static_cast<void>(signing.get()));
      BOOST_CHECK(locking.get().status == wallet::state::locked);
      BOOST_CHECK(run(runtime, value.status()).status == wallet::state::locked);
      BOOST_CHECK(run(runtime, value.unlock("secret")).status == wallet::state::unlocked);
      run(runtime, value.shutdown());
   }
}

BOOST_AUTO_TEST_CASE(queue_refusal_does_not_partially_revoke_wallet) {
   temporary_directory directory;
   forge::asio::runtime runtime;
   forge::asio::task::scheduler scheduler{runtime, {.max_blocking_tasks = 1}};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   plugin::provider value{"producer"};
   value.initialize(directory.path, scheduler, compute.get_executor(), 0, 1);
   value.startup();
   run(runtime, value.create("secret"));
   run(runtime, value.unlock("secret"));
   run(runtime, value.create_key("key"));
   blocking_pool_guard busy{runtime, compute.get_executor()};
   auto signing =
       boost::asio::co_spawn(runtime.context(), value.sign_digest({.id = {.value = "key"}}), boost::asio::use_future);
   flush(runtime);
   auto status = boost::asio::co_spawn(runtime.context(), value.status(), boost::asio::use_future);
   flush(runtime);
   BOOST_CHECK_THROW(run(runtime, value.lock()), wallet::exceptions::resource_exhausted);
   BOOST_CHECK_THROW(run(runtime, value.open()), wallet::exceptions::resource_exhausted);
   busy.finish();
   BOOST_CHECK_NO_THROW(static_cast<void>(signing.get()));
   BOOST_CHECK(status.get().status == wallet::state::unlocked);
   BOOST_CHECK_NO_THROW(run(runtime, value.sign_digest({.id = {.value = "key"}})));
   run(runtime, value.shutdown());
}

BOOST_AUTO_TEST_CASE(manual_lock_wins_over_an_inflight_unlock_without_automatic_reunlock) {
   temporary_directory directory;
   forge::asio::runtime runtime;
   forge::asio::task::scheduler scheduler{runtime};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   plugin::provider value{"producer"};
   value.initialize(directory.path, scheduler, compute.get_executor(), 0);
   value.startup();
   run(runtime, value.create("secret"));
   blocking_pool_guard busy{runtime, compute.get_executor()};
   auto unlocking = boost::asio::co_spawn(runtime.context(), value.unlock("secret"), boost::asio::use_future);
   flush(runtime);
   auto locking = boost::asio::co_spawn(runtime.context(), value.lock(), boost::asio::use_future);
   flush(runtime);
   busy.finish();
   BOOST_CHECK(unlocking.get().status == wallet::state::locked);
   BOOST_CHECK(locking.get().status == wallet::state::locked);
   BOOST_CHECK(run(runtime, value.status()).status == wallet::state::locked);
   BOOST_CHECK(run(runtime, value.unlock("secret")).status == wallet::state::unlocked);
   run(runtime, value.shutdown());
}

BOOST_AUTO_TEST_CASE(limits_and_dependency_errors_do_not_disclose_secret_input) {
   auto config = permissions({"alice", "bob"}, {wallet::operation::create, wallet::operation::unlock,
                                                wallet::operation::import_key, wallet::operation::list});
   config.max_wallets = 1;
   plugin_fixture f{std::move(config)};
   auto api = f.api();
   run(f.runtime, api->create({"alice", "secret"}, caller()));
   BOOST_CHECK_THROW(run(f.runtime, api->create({"bob", "secret"}, caller())), wallet::exceptions::resource_exhausted);
   BOOST_CHECK_THROW(run(f.runtime, api->unlock({"alice", std::string(4097, 'x')}, caller())),
                     wallet::exceptions::invalid_request);
   run(f.runtime, api->unlock({"alice", "secret"}, caller()));
   try {
      run(f.runtime, api->import_key({"alice", "key", "sensitive-invalid-key"}, caller()));
      BOOST_FAIL("Invalid private key accepted");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(std::string{error.what()}.find("sensitive-invalid-key") == std::string::npos);
      BOOST_TEST(std::string{error.what()}.find(f.directory.path.string()) == std::string::npos);
   }
}

BOOST_AUTO_TEST_CASE(startup_unlock_is_explicit_and_runs_only_once) {
   auto settings =
       permissions({"service"}, {wallet::operation::status, wallet::operation::lock, wallet::operation::unlock});
   plugin_fixture f{std::move(settings), [](auto& config, const auto& directory) {
                       static_cast<void>(keystore::store::create(directory / "service.fks", "test-password"));
                       const auto file = directory / "startup-password";
                       std::ofstream{file} << "test-password\n";
                       std::filesystem::permissions(file, std::filesystem::perms::owner_read |
                                                              std::filesystem::perms::owner_write);
                       config.wallets.push_back(
                           {.name = "service", .open = true, .timeout_seconds = 0, .unlock_file = file.string()});
                    }};
   const auto api = f.api();
   BOOST_CHECK(run(f.runtime, api->status({"service"}, caller())).status == wallet::state::unlocked);
   BOOST_CHECK(run(f.runtime, api->lock({"service"}, caller())).status == wallet::state::locked);
   flush(f.runtime);
   BOOST_CHECK(run(f.runtime, api->status({"service"}, caller())).status == wallet::state::locked);
   BOOST_CHECK(run(f.runtime, api->unlock({"service", "test-password"}, caller())).status == wallet::state::unlocked);
}

BOOST_AUTO_TEST_CASE(one_scheduler_slot_supports_multiple_wallets_and_expiry_during_signing) {
   temporary_directory directory;
   forge::asio::runtime runtime{{.worker_threads = 1}};
   forge::asio::task::scheduler scheduler{runtime, {.max_awaitable_tasks = 1, .max_pending_tasks = 4}};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   plugin::provider first{"first"};
   plugin::provider second{"second"};
   for (auto* wallet : {&first, &second}) {
      wallet->initialize(directory.path, scheduler, compute.get_executor(), 1);
      wallet->startup();
      auto creating =
          boost::asio::co_spawn(runtime.context(), wallet->create("test-password"), boost::asio::use_future);
      BOOST_REQUIRE(creating.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
      BOOST_CHECK(creating.get().status == wallet::state::locked);
      run(runtime, wallet->unlock("test-password"));
      run(runtime, wallet->create_key("key"));
   }
   for (unsigned index = 0; index < 20; ++index) {
      run(runtime, first.sign_digest({.id = {.value = "key"}}));
   }
   BOOST_CHECK(scheduler.pending_count() <= 2U);
   blocking_pool_guard busy{runtime, compute.get_executor()};
   auto signing =
       boost::asio::co_spawn(runtime.context(), first.sign_digest({.id = {.value = "key"}}), boost::asio::use_future);
   flush(runtime);
   std::this_thread::sleep_for(std::chrono::milliseconds{1100});
   busy.finish();
   BOOST_REQUIRE(signing.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(static_cast<void>(signing.get()));
   BOOST_CHECK(run(runtime, first.status()).status == wallet::state::unlocked);
   BOOST_CHECK(run(runtime, second.status()).status == wallet::state::locked);
   run(runtime, first.shutdown());
   run(runtime, second.shutdown());
}

BOOST_AUTO_TEST_CASE(idle_task_admission_refusal_does_not_leave_an_unprotected_unlocked_wallet) {
   temporary_directory directory;
   forge::asio::runtime runtime{{.worker_threads = 1}};
   forge::asio::task::scheduler scheduler{runtime, {.max_awaitable_tasks = 1, .max_pending_tasks = 1}};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   plugin::provider value{"producer"};
   value.initialize(directory.path, scheduler, compute.get_executor());
   value.startup();
   run(runtime, value.create("secret"));
   auto occupied = scheduler.submit_after(forge::asio::task::task{.name = "wallet.test.full", .work = [] {}},
                                          std::chrono::hours{1});
   BOOST_CHECK_THROW(run(runtime, value.unlock("secret")), wallet::exceptions::resource_exhausted);
   BOOST_CHECK(run(runtime, value.status()).status == wallet::state::locked);
   BOOST_CHECK_THROW(run(runtime, value.sign_digest({.id = {.value = "key"}})), wallet::exceptions::locked);
   occupied.cancel();
   run(runtime, value.shutdown());
}

BOOST_AUTO_TEST_CASE(timeout_changes_reuse_a_single_pending_scheduler_slot) {
   temporary_directory directory;
   forge::asio::runtime runtime{{.worker_threads = 1}};
   forge::asio::task::scheduler scheduler{runtime, {.max_awaitable_tasks = 1, .max_pending_tasks = 1}};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   plugin::provider value{"producer"};
   value.initialize(directory.path, scheduler, compute.get_executor(), 300);
   value.startup();
   run(runtime, value.create("secret"));
   run(runtime, value.unlock("secret"));
   BOOST_CHECK_EQUAL(scheduler.pending_count(), 1U);
   BOOST_CHECK(run(runtime, value.set_timeout(1)).status == wallet::state::unlocked);
   BOOST_CHECK_EQUAL(scheduler.pending_count(), 1U);
   BOOST_CHECK(run(runtime, value.set_timeout(0)).status == wallet::state::unlocked);
   BOOST_CHECK_EQUAL(scheduler.pending_count(), 0U);
   BOOST_CHECK(run(runtime, value.set_timeout(1)).status == wallet::state::unlocked);
   std::this_thread::sleep_for(std::chrono::milliseconds{1100});
   BOOST_CHECK(run(runtime, value.status()).status == wallet::state::locked);
   BOOST_CHECK(run(runtime, value.unlock("secret")).status == wallet::state::unlocked);
   run(runtime, value.shutdown());
   BOOST_CHECK_EQUAL(scheduler.pending_count(), 0U);
}

BOOST_AUTO_TEST_CASE(relocking_releases_the_idle_slot_for_another_wallet) {
   temporary_directory directory;
   forge::asio::runtime runtime{{.worker_threads = 2}};
   forge::asio::task::scheduler scheduler{runtime, {.max_awaitable_tasks = 1, .max_pending_tasks = 1}};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   plugin::provider first{"first"};
   plugin::provider second{"second"};
   for (auto* value : {&first, &second}) {
      value->initialize(directory.path, scheduler, compute.get_executor(), 300);
      value->startup();
      run(runtime, value->create("secret"));
   }
   for (const bool reopen : {false, true}) {
      run(runtime, first.unlock("secret"));
      BOOST_CHECK_EQUAL(scheduler.pending_count(), 1U);
      const auto result = reopen ? run(runtime, first.open()) : run(runtime, first.lock());
      BOOST_CHECK(result.status == wallet::state::locked);
      BOOST_CHECK_EQUAL(scheduler.pending_count(), 0U);
      BOOST_CHECK(run(runtime, second.unlock("secret")).status == wallet::state::unlocked);
      BOOST_CHECK_EQUAL(scheduler.pending_count(), 1U);
      run(runtime, second.lock());
   }
   run(runtime, first.shutdown());
   run(runtime, second.shutdown());
}

BOOST_AUTO_TEST_CASE(list_and_lock_all_respect_their_own_wallet_permissions) {
   auto settings =
       permissions({"alice", "bob"}, {wallet::operation::create, wallet::operation::unlock, wallet::operation::status});
   settings.permissions.push_back({.fingerprint = caller().fingerprint.str(),
                                   .wallets = {"alice"},
                                   .operations = {wallet::operation::list, wallet::operation::lock_all}});
   plugin_fixture f{std::move(settings)};
   auto api = f.api();
   for (const auto* name : {"alice", "bob"}) {
      run(f.runtime, api->create({name, "test-password"}, caller()));
      run(f.runtime, api->unlock({name, "test-password"}, caller()));
   }
   const auto listed = run(f.runtime, api->list(caller()));
   BOOST_REQUIRE_EQUAL(listed.size(), 1U);
   BOOST_TEST(listed.front().wallet == "alice");
   const auto locked = run(f.runtime, api->lock_all(caller()));
   BOOST_REQUIRE_EQUAL(locked.size(), 1U);
   BOOST_TEST(locked.front().wallet == "alice");
   BOOST_CHECK(run(f.runtime, api->status({"alice"}, caller())).status == wallet::state::locked);
   BOOST_CHECK(run(f.runtime, api->status({"bob"}, caller())).status == wallet::state::unlocked);
}

BOOST_AUTO_TEST_CASE(shutdown_drains_inflight_signing_despite_cancellation) {
   temporary_directory directory;
   forge::asio::runtime runtime;
   forge::asio::task::scheduler scheduler{runtime};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   plugin::provider value{"producer"};
   value.initialize(directory.path, scheduler, compute.get_executor(), 0);
   value.startup();
   run(runtime, value.create("secret"));
   run(runtime, value.unlock("secret"));
   run(runtime, value.create_key("key"));
   blocking_pool_guard busy{runtime, compute.get_executor()};
   auto signing =
       boost::asio::co_spawn(runtime.context(), value.sign_digest({.id = {.value = "key"}}), boost::asio::use_future);
   flush(runtime);
   auto cancellation = boost::asio::cancellation_signal{};
   auto stopping =
       boost::asio::co_spawn(runtime.context(), value.shutdown(),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   flush(runtime);
   cancellation.emit(boost::asio::cancellation_type::all);
   flush(runtime);
   BOOST_CHECK(stopping.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   busy.finish();
   BOOST_CHECK_NO_THROW(static_cast<void>(signing.get()));
   BOOST_CHECK_NO_THROW(stopping.get());
   BOOST_CHECK_NO_THROW(static_cast<void>(keystore::store::open(directory.path / "producer.fks", "secret")));
}

BOOST_AUTO_TEST_CASE(invalid_names_and_startup_unlock_policy_are_rejected) {
   BOOST_CHECK_THROW(plugin::provider{"../escape"}, wallet::exceptions::invalid_request);
   BOOST_CHECK_THROW(plugin::provider{std::string(65, 'a')}, wallet::exceptions::invalid_request);
   auto config = plugin::config{.wallets = {{.name = "alice", .open = true, .unlock_file = "/not/read"}}};
   BOOST_CHECK_THROW(plugin_fixture{std::move(config)}, wallet::exceptions::invalid_request);
}

BOOST_AUTO_TEST_SUITE_END()
