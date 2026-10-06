#include <boost/test/unit_test.hpp>

#include <array>
#include <memory>
#include <new>
#include <string_view>

#include "../../libraries/net/quic/details/engine_listener_impl.hxx"
#include "../../libraries/net/quic/details/engine_connection_impl.hxx"

namespace {
namespace asio = boost::asio;
namespace detail = forge::net::quic::detail;

constexpr auto registry_faults = std::array{
    std::string_view{"cid_registry_before_reverse_index_insert"},
    std::string_view{"cid_registry_before_reverse_index_append"},
    std::string_view{"cid_registry_before_forward_index_insert"},
};

struct cid_registry_fixture {
   asio::io_context context;
   detail::engine_listener::impl registry{context, {.host = "127.0.0.1"}, detail::engine_server_options{}};

   std::shared_ptr<detail::engine_connection::impl> connection() {
      return std::make_shared<detail::engine_connection::impl>(
          context, registry.server_socket, asio::ip::udp::endpoint{asio::ip::address_v4::loopback(), 34000},
          asio::ip::udp::endpoint{asio::ip::address_v4::loopback(), 34001}, detail::engine_transport_limits{});
   }
};

BOOST_AUTO_TEST_CASE(quic_cid_registration_bad_alloc_retains_neither_index_nor_strong_owner) {
   // Exercise the real private registry, without starting native workers. Both
   // indexes and weak ownership must be unchanged before any listener stop.
   for (const auto point : registry_faults) {
      BOOST_TEST_CONTEXT(point) {
         auto state = cid_registry_fixture{};
         auto connection = state.connection();
         const auto lifetime = std::weak_ptr{connection};
         auto calls = std::size_t{0};
         connection->test_failpoint = [point, &calls](std::string_view name) -> bool {
            if (name == point) {
               ++calls;
               throw std::bad_alloc{};
            }
            return false;
         };
         BOOST_CHECK_THROW(state.registry.register_connection_cid(connection, "first"), std::bad_alloc);
         BOOST_TEST(calls == 1U);
         BOOST_TEST(state.registry.connections_by_cid.empty());
         BOOST_TEST(state.registry.cids_by_connection.empty());
         BOOST_TEST(state.registry.connection_count() == 0U);
         BOOST_TEST(state.registry.connections().empty());
         BOOST_TEST(!state.registry.find_connection_by_cid("first"));
         connection.reset();
         BOOST_TEST(lifetime.expired());

         auto retry = state.connection();
         state.registry.register_connection_cid(retry, "first");
         BOOST_TEST(state.registry.connection_count() == 1U);
         BOOST_TEST(state.registry.find_connection_by_cid("first") == retry);
         BOOST_TEST(state.registry.release_connection_slot(retry.get()));
         BOOST_TEST(state.registry.connections_by_cid.empty());
         BOOST_TEST(state.registry.cids_by_connection.empty());
      }
   }
}

BOOST_AUTO_TEST_CASE(quic_cid_registration_bad_alloc_preserves_existing_connection_and_ids) {
   for (const auto point : registry_faults) {
      BOOST_TEST_CONTEXT(point) {
         auto state = cid_registry_fixture{};
         auto connection = state.connection();
         state.registry.register_connection_cid(connection, "first");
         connection->test_failpoint = [point](std::string_view name) -> bool {
            if (name == point) {
               throw std::bad_alloc{};
            }
            return false;
         };
         BOOST_CHECK_THROW(state.registry.register_connection_cid(connection, "second"), std::bad_alloc);
         BOOST_TEST(state.registry.connections_by_cid.size() == 1U);
         BOOST_TEST(state.registry.cids_by_connection.size() == 1U);
         BOOST_TEST(state.registry.connection_count() == 1U);
         BOOST_TEST(state.registry.find_connection_by_cid("first") == connection);
         BOOST_TEST(!state.registry.find_connection_by_cid("second"));
         const auto& keys = state.registry.cids_by_connection.at(connection.get());
         BOOST_REQUIRE(keys.size() == 1U);
         BOOST_TEST(keys.front() == "first");

         connection->test_failpoint = {};
         state.registry.register_connection_cid(connection, "second");
         BOOST_TEST(state.registry.connection_count() == 1U);
         BOOST_TEST(state.registry.find_connection_by_cid("second") == connection);
         BOOST_TEST(state.registry.cids_by_connection.at(connection.get()).size() == 2U);
         BOOST_TEST(state.registry.release_connection_slot(connection.get()));
         BOOST_TEST(state.registry.connections_by_cid.empty());
         BOOST_TEST(state.registry.cids_by_connection.empty());
      }
   }
}

BOOST_AUTO_TEST_CASE(quic_cid_registration_is_idempotent_and_cannot_replace_another_owner) {
   auto state = cid_registry_fixture{};
   auto connection = state.connection();
   state.registry.register_connection_cid(connection, "first");
   connection->test_failpoint = [](std::string_view) -> bool { throw std::bad_alloc{}; };
   BOOST_CHECK_NO_THROW(state.registry.register_connection_cid(connection, "first"));
   BOOST_TEST(state.registry.cids_by_connection.at(connection.get()).size() == 1U);

   auto other = state.connection();
   const auto lifetime = std::weak_ptr{other};
   BOOST_CHECK_EXCEPTION(state.registry.register_connection_cid(other, "first"), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::internal_error; });
   BOOST_TEST(state.registry.find_connection_by_cid("first") == connection);
   BOOST_TEST(state.registry.connection_count() == 1U);
   BOOST_TEST(state.registry.cids_by_connection.size() == 1U);
   other.reset();
   BOOST_TEST(lifetime.expired());
   state.registry.unregister_connection_cid(connection.get(), "first");
   BOOST_TEST(state.registry.connections_by_cid.empty());
   BOOST_TEST(state.registry.cids_by_connection.empty());
}

} // namespace
