#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/db/object/macros.hpp>

#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

import forge.asio.affine;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.auth.pairing.pairing;
import forge.db.mdbx.driver;
import forge.db.object.exceptions;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.store;
import forge.db.object.transaction;
import forge.exceptions;

namespace auth_mdbx_tests {

namespace credentials = forge::auth::credential;
namespace pairing = forge::auth::pairing;

struct by_auth_id;
struct by_audit_id;

struct auth_record : forge::db::object::object<auth_record, 253, 1> {
   std::string record_type;
   std::string identity;
   credentials::scope_set scopes;
   credentials::scope_set scope_baseline;
   forge::crypto::digest::sha256 digest;
   std::int64_t created_at = 0;
   std::int64_t expires_at = 0;
   std::int64_t updated_at = 0;
   std::optional<std::int64_t> resolved_at;
   std::string credential_id;
   std::uint64_t generation = 0;
   std::uint8_t state = 0;
   bool consumed = false;
   bool pre_session_consumed = false;

   bool operator==(const auth_record&) const = default;
};

BOOST_DESCRIBE_STRUCT(auth_record, (forge::db::object::object<auth_record, 253, 1>),
                      (record_type, identity, scopes, scope_baseline, digest, created_at, expires_at, updated_at,
                       resolved_at, credential_id, generation, state, consumed, pre_session_consumed))

using auth_index =
    forge::db::object::object_index<auth_record,
                                    forge::db::object::indexed_by<forge::db::object::primary_unique<by_auth_id>>>;

struct audit_record : forge::db::object::object<audit_record, 253, 2> {
   std::string operation;
   std::string subject;
   std::uint64_t generation = 0;

   bool operator==(const audit_record&) const = default;
};

BOOST_DESCRIBE_STRUCT(audit_record, (forge::db::object::object<audit_record, 253, 2>), (operation, subject, generation))

using audit_index =
    forge::db::object::object_index<audit_record,
                                    forge::db::object::indexed_by<forge::db::object::primary_unique<by_audit_id>>>;

} // namespace auth_mdbx_tests

FORGE_DB_OBJECT(auth_mdbx_tests::auth_index)
FORGE_DB_OBJECT(auth_mdbx_tests::audit_index)

namespace {

namespace credentials = forge::auth::credential;
namespace pairing = forge::auth::pairing;
using auth_mdbx_tests::audit_record;
using auth_mdbx_tests::auth_record;

constexpr auto bootstrap_id = auth_record::id_t{1};
constexpr auto consumed_pending_id = auth_record::id_t{2};
constexpr auto approval_pending_id = auth_record::id_t{3};
constexpr auto approved_credential_id = auth_record::id_t{4};
constexpr auto rotating_credential_id = auth_record::id_t{5};
constexpr auto rollback_credential_id = auth_record::id_t{6};

constexpr auto consume_audit_id = audit_record::id_t{1};
constexpr auto approve_audit_id = audit_record::id_t{2};
constexpr auto rotate_audit_id = audit_record::id_t{3};
constexpr auto rollback_audit_id = audit_record::id_t{4};

constexpr auto test_now = pairing::time_point{std::chrono::seconds{1'700'000'000}};

[[nodiscard]] std::int64_t encode_time(pairing::time_point value) {
   return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

[[nodiscard]] pairing::time_point decode_time(std::int64_t value) {
   return pairing::time_point{
       std::chrono::duration_cast<pairing::time_point::duration>(std::chrono::nanoseconds{value})};
}

[[nodiscard]] std::optional<std::int64_t> encode_time(const std::optional<pairing::time_point>& value) {
   return value.has_value() ? std::optional<std::int64_t>{encode_time(*value)} : std::nullopt;
}

[[nodiscard]] std::optional<pairing::time_point> decode_time(const std::optional<std::int64_t>& value) {
   return value.has_value() ? std::optional<pairing::time_point>{decode_time(*value)} : std::nullopt;
}

[[nodiscard]] auth_record store_bootstrap(auth_record::id_t id, const pairing::bootstrap_record& value) {
   auto result = auth_record{};
   result.id = id;
   result.record_type = "bootstrap";
   result.scopes = value.scope_baseline;
   result.digest = value.digest.value;
   result.created_at = encode_time(value.created_at);
   result.expires_at = encode_time(value.expires_at);
   result.consumed = value.consumed;
   return result;
}

[[nodiscard]] pairing::bootstrap_record load_bootstrap(const auth_record& value) {
   BOOST_REQUIRE_EQUAL(value.record_type, "bootstrap");
   return {
       .digest = {.value = value.digest},
       .scope_baseline = value.scopes,
       .created_at = decode_time(value.created_at),
       .expires_at = decode_time(value.expires_at),
       .consumed = value.consumed,
   };
}

[[nodiscard]] auth_record store_pending(auth_record::id_t id, const pairing::pending_request& value) {
   auto result = auth_record{};
   result.id = id;
   result.record_type = "pending";
   result.identity = value.identity;
   result.scopes = value.requested_scopes;
   result.scope_baseline = value.scope_baseline;
   result.digest = value.pre_session_digest.value;
   result.created_at = encode_time(value.created_at);
   result.expires_at = encode_time(value.expires_at);
   result.resolved_at = encode_time(value.resolved_at);
   result.state = static_cast<std::uint8_t>(value.state);
   result.pre_session_consumed = value.pre_session_consumed;
   if (value.approved_credential.has_value()) {
      result.credential_id = value.approved_credential->id.value;
      result.generation = value.approved_credential->generation;
   }
   return result;
}

[[nodiscard]] pairing::pending_request load_pending(const auth_record& value) {
   BOOST_REQUIRE_EQUAL(value.record_type, "pending");
   auto result = pairing::pending_request{
       .identity = value.identity,
       .requested_scopes = value.scopes,
       .scope_baseline = value.scope_baseline,
       .pre_session_digest = {.value = value.digest},
       .created_at = decode_time(value.created_at),
       .expires_at = decode_time(value.expires_at),
       .state = static_cast<pairing::pending_state>(value.state),
       .resolved_at = decode_time(value.resolved_at),
       .pre_session_consumed = value.pre_session_consumed,
   };
   if (!value.credential_id.empty()) {
      result.approved_credential = credentials::binding{
          .id = {.value = value.credential_id},
          .generation = value.generation,
      };
   }
   return result;
}

[[nodiscard]] auth_record store_credential(auth_record::id_t id, const credentials::record& value) {
   auto result = auth_record{};
   result.id = id;
   result.record_type = "credential";
   result.identity = value.identity;
   result.scopes = value.scopes;
   result.credential_id = value.id.value;
   result.generation = value.generation;
   result.created_at = encode_time(value.issued_at);
   result.updated_at = encode_time(value.updated_at);
   result.state = static_cast<std::uint8_t>(value.state);
   result.resolved_at = encode_time(value.revoked_at);
   return result;
}

[[nodiscard]] credentials::record load_credential(const auth_record& value) {
   BOOST_REQUIRE_EQUAL(value.record_type, "credential");
   return {
       .id = {.value = value.credential_id},
       .identity = value.identity,
       .scopes = value.scopes,
       .generation = value.generation,
       .issued_at = decode_time(value.created_at),
       .updated_at = decode_time(value.updated_at),
       .state = static_cast<credentials::state>(value.state),
       .revoked_at = decode_time(value.resolved_at),
   };
}

[[nodiscard]] audit_record make_audit(audit_record::id_t id, std::string operation, std::string subject,
                                      std::uint64_t generation) {
   auto result = audit_record{};
   result.id = id;
   result.operation = std::move(operation);
   result.subject = std::move(subject);
   result.generation = generation;
   return result;
}

[[nodiscard]] std::filesystem::path make_root() {
   const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
   auto root = std::filesystem::temp_directory_path() / ("forge_auth_mdbx_" + suffix);
   std::filesystem::remove_all(root);
   return root;
}

struct root_cleanup {
   std::filesystem::path path;

   ~root_cleanup() {
      auto ignored = std::error_code{};
      std::filesystem::remove_all(path, ignored);
   }
};

[[nodiscard]] forge::db::mdbx::config database_config(const std::filesystem::path& path) {
   return {
       .path = path.string(),
       .families = {"objectdb"},
   };
}

boost::asio::awaitable<forge::db::object::store> open_store(const std::shared_ptr<forge::db::mdbx::driver>& driver) {
   auto objects = co_await forge::db::object::store::open(
       driver, forge::db::object::store::options{.writes = forge::db::object::write_policy::backend});
   objects.register_object<auth_mdbx_tests::auth_index>();
   objects.register_object<auth_mdbx_tests::audit_index>();
   co_return objects;
}

boost::asio::awaitable<void> rollback_safely(forge::db::object::transaction& transaction) {
   try {
      co_await transaction.rollback();
   } catch (const forge::exceptions::base&) {
   }
}

template <typename Exception> [[nodiscard]] bool is_exception(const std::exception_ptr& error) {
   try {
      std::rethrow_exception(error);
   } catch (const Exception&) {
      return true;
   } catch (...) {
      return false;
   }
}

boost::asio::awaitable<bool> consume_once(forge::db::object::store objects, forge::crypto::core::secret_string token,
                                          std::barrier<>& start) {
   start.arrive_and_wait();
   auto transaction = co_await objects.begin_transaction();
   auto error = std::exception_ptr{};
   try {
      auto stored = co_await transaction.get(bootstrap_id);
      auto bootstrap = load_bootstrap(stored);
      auto pending = pairing::consume_bootstrap(bootstrap, token,
                                                {.identity = "browser-owner", .requested_scopes = {"admin.read"}},
                                                {.now = test_now + std::chrono::seconds{1},
                                                 .request_expires_at = test_now + std::chrono::minutes{2},
                                                 .pending_count = 0,
                                                 .max_pending_requests = 4});
      co_await transaction.replace(store_bootstrap(bootstrap_id, bootstrap));
      co_await transaction.insert(store_pending(consumed_pending_id, pending.record));
      co_await transaction.insert(make_audit(consume_audit_id, "consume", pending.record.identity, 0));
      co_await transaction.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (!error) {
      co_return true;
   }
   co_await rollback_safely(transaction);
   if (is_exception<pairing::exceptions::replayed>(error)) {
      co_return false;
   }
   std::rethrow_exception(error);
}

boost::asio::awaitable<bool> approve_once(forge::db::object::store objects, std::barrier<>& start) {
   start.arrive_and_wait();
   auto transaction = co_await objects.begin_transaction();
   auto error = std::exception_ptr{};
   try {
      auto stored = co_await transaction.get(approval_pending_id);
      auto pending = load_pending(stored);
      auto credential = pairing::approve_pending(
          pending, {.id = {.value = "owner-credential"}, .now = test_now + std::chrono::seconds{3}});
      co_await transaction.replace(store_pending(approval_pending_id, pending));
      co_await transaction.insert(store_credential(approved_credential_id, credential));
      co_await transaction.insert(make_audit(approve_audit_id, "approve", credential.id.value, credential.generation));
      co_await transaction.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (!error) {
      co_return true;
   }
   co_await rollback_safely(transaction);
   if (is_exception<pairing::exceptions::invalid_state>(error)) {
      co_return false;
   }
   std::rethrow_exception(error);
}

boost::asio::awaitable<bool> rotate_once(forge::db::object::store objects, std::barrier<>& start) {
   start.arrive_and_wait();
   auto transaction = co_await objects.begin_transaction();
   auto error = std::exception_ptr{};
   try {
      auto stored = co_await transaction.get(rotating_credential_id);
      auto credential = load_credential(stored);
      credentials::rotate_downscope(credential, {"admin.read"}, test_now + std::chrono::seconds{4});
      co_await transaction.replace(store_credential(rotating_credential_id, credential));
      co_await transaction.insert(make_audit(rotate_audit_id, "rotate", credential.id.value, credential.generation));
      co_await transaction.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (!error) {
      co_return true;
   }
   co_await rollback_safely(transaction);
   if (is_exception<credentials::exceptions::invalid_state>(error)) {
      co_return false;
   }
   std::rethrow_exception(error);
}

boost::asio::awaitable<void> rotate_with_colliding_audit(forge::db::object::store objects) {
   auto transaction = co_await objects.begin_transaction();
   auto error = std::exception_ptr{};
   try {
      auto stored = co_await transaction.get(rollback_credential_id);
      auto credential = load_credential(stored);
      credentials::rotate_downscope(credential, {"admin.read"}, test_now + std::chrono::seconds{5});
      co_await transaction.replace(store_credential(rollback_credential_id, credential));
      co_await transaction.insert(
          make_audit(rollback_audit_id, "rollback-collision", credential.id.value, credential.generation));
      co_await transaction.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await rollback_safely(transaction);
      std::rethrow_exception(error);
   }
}

[[nodiscard]] bool exactly_one(bool first, bool second) {
   return first != second;
}

} // namespace

BOOST_AUTO_TEST_SUITE(auth_mdbx_integration)

BOOST_AUTO_TEST_CASE(concurrent_pairing_transitions_commit_state_and_audit_atomically) {
   const auto root = make_root();
   const auto cleanup = root_cleanup{root};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lane = forge::asio::affine::lane{};
   auto driver = forge::asio::blocking::run(
       runtime, forge::db::mdbx::driver::open(database_config(root / "store"), lane.get_executor()));
   auto objects = std::optional{forge::asio::blocking::run(runtime, open_store(driver))};

   const auto bootstrap = pairing::begin_bootstrap({
       .now = test_now,
       .expires_at = test_now + std::chrono::minutes{10},
       .scope_baseline = {"admin.read", "admin.write"},
   });
   auto approval_bootstrap = pairing::begin_bootstrap({
       .now = test_now,
       .expires_at = test_now + std::chrono::minutes{10},
       .scope_baseline = {"admin.read", "admin.write"},
   });
   const auto approval_pending =
       pairing::consume_bootstrap(approval_bootstrap.record, approval_bootstrap.token,
                                  {.identity = "approval-owner", .requested_scopes = {"admin.read", "admin.write"}},
                                  {.now = test_now + std::chrono::seconds{1},
                                   .request_expires_at = test_now + std::chrono::minutes{2},
                                   .pending_count = 0,
                                   .max_pending_requests = 4});
   const auto rotating = credentials::record{
       .id = {.value = "rotating-credential"},
       .identity = "rotation-owner",
       .scopes = {"admin.read", "admin.write"},
       .generation = 1,
       .issued_at = test_now,
       .updated_at = test_now,
   };
   const auto rollback = credentials::record{
       .id = {.value = "rollback-credential"},
       .identity = "rollback-owner",
       .scopes = {"admin.read", "admin.write"},
       .generation = 1,
       .issued_at = test_now,
       .updated_at = test_now,
   };

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto transaction = co_await objects->begin_transaction();
      co_await transaction.insert(store_bootstrap(bootstrap_id, bootstrap.record));
      co_await transaction.insert(store_pending(approval_pending_id, approval_pending.record));
      co_await transaction.insert(store_credential(rotating_credential_id, rotating));
      co_await transaction.insert(store_credential(rollback_credential_id, rollback));
      co_await transaction.insert(make_audit(rollback_audit_id, "occupied", "rollback", 0));
      co_await transaction.commit();
   }());

   auto consume_start = std::barrier{2};
   auto first_consume = boost::asio::co_spawn(runtime.context(), consume_once(*objects, bootstrap.token, consume_start),
                                              boost::asio::use_future);
   auto second_consume = boost::asio::co_spawn(
       runtime.context(), consume_once(*objects, bootstrap.token, consume_start), boost::asio::use_future);
   BOOST_CHECK(exactly_one(first_consume.get(), second_consume.get()));

   auto approve_start = std::barrier{2};
   auto first_approve =
       boost::asio::co_spawn(runtime.context(), approve_once(*objects, approve_start), boost::asio::use_future);
   auto second_approve =
       boost::asio::co_spawn(runtime.context(), approve_once(*objects, approve_start), boost::asio::use_future);
   BOOST_CHECK(exactly_one(first_approve.get(), second_approve.get()));

   auto rotate_start = std::barrier{2};
   auto first_rotate =
       boost::asio::co_spawn(runtime.context(), rotate_once(*objects, rotate_start), boost::asio::use_future);
   auto second_rotate =
       boost::asio::co_spawn(runtime.context(), rotate_once(*objects, rotate_start), boost::asio::use_future);
   BOOST_CHECK(exactly_one(first_rotate.get(), second_rotate.get()));

   auto collision =
       boost::asio::co_spawn(runtime.context(), rotate_with_colliding_audit(*objects), boost::asio::use_future);
   BOOST_CHECK_THROW(collision.get(), forge::db::object::exceptions::duplicate_object);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto consumed = load_bootstrap(co_await objects->get(bootstrap_id));
      BOOST_CHECK(consumed.consumed);
      const auto approved = load_pending(co_await objects->get(approval_pending_id));
      BOOST_CHECK(approved.state == pairing::pending_state::approved);
      BOOST_REQUIRE(approved.approved_credential.has_value());
      BOOST_CHECK_EQUAL(approved.approved_credential->generation, 1U);
      const auto rotated = load_credential(co_await objects->get(rotating_credential_id));
      BOOST_CHECK_EQUAL(rotated.generation, 2U);
      BOOST_CHECK(rotated.scopes == credentials::scope_set{"admin.read"});
      const auto rolled_back = load_credential(co_await objects->get(rollback_credential_id));
      BOOST_CHECK(rolled_back == rollback);
      const auto consume_audit = co_await objects->get(consume_audit_id);
      const auto approve_audit = co_await objects->get(approve_audit_id);
      const auto rotate_audit = co_await objects->get(rotate_audit_id);
      const auto rollback_audit = co_await objects->get(rollback_audit_id);
      BOOST_CHECK(consume_audit == make_audit(consume_audit_id, "consume", "browser-owner", 0));
      BOOST_CHECK(approve_audit == make_audit(approve_audit_id, "approve", "owner-credential", 1));
      BOOST_CHECK(rotate_audit == make_audit(rotate_audit_id, "rotate", "rotating-credential", 2));
      BOOST_CHECK(rollback_audit == make_audit(rollback_audit_id, "occupied", "rollback", 0));
   }());

   objects.reset();
   forge::asio::blocking::run(runtime, driver->async_flush(true));
   forge::asio::blocking::run(runtime, driver->async_close());
   driver.reset();

   driver = forge::asio::blocking::run(
       runtime, forge::db::mdbx::driver::open(database_config(root / "store"), lane.get_executor()));
   objects.emplace(forge::asio::blocking::run(runtime, open_store(driver)));
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      BOOST_CHECK((co_await objects->get(bootstrap_id)).consumed);
      BOOST_CHECK_EQUAL((co_await objects->get(approved_credential_id)).generation, 1U);
      BOOST_CHECK_EQUAL((co_await objects->get(rotating_credential_id)).generation, 2U);
      BOOST_CHECK_EQUAL((co_await objects->get(rollback_credential_id)).generation, 1U);
      const auto consume_audit = co_await objects->get(consume_audit_id);
      const auto approve_audit = co_await objects->get(approve_audit_id);
      const auto rotate_audit = co_await objects->get(rotate_audit_id);
      const auto rollback_audit = co_await objects->get(rollback_audit_id);
      BOOST_CHECK(consume_audit == make_audit(consume_audit_id, "consume", "browser-owner", 0));
      BOOST_CHECK(approve_audit == make_audit(approve_audit_id, "approve", "owner-credential", 1));
      BOOST_CHECK(rotate_audit == make_audit(rotate_audit_id, "rotate", "rotating-credential", 2));
      BOOST_CHECK(rollback_audit == make_audit(rollback_audit_id, "occupied", "rollback", 0));
   }());

   objects.reset();
   forge::asio::blocking::run(runtime, driver->async_close());
   driver.reset();
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_SUITE_END()
