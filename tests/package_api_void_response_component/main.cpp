#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <typeindex>
#include <utility>
#include <vector>

import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.dispatcher;
import forge.api.core.error_projection;
import forge.api.core.exceptions;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.asio.blocking;
import forge.asio.runtime;

namespace core = forge::api::core;

class void_api : public core::contract<void_api, core::surface::local | core::surface::remote> {
 public:
   virtual ~void_api() = default;
   virtual boost::asio::awaitable<void> zero() = 0;
   virtual boost::asio::awaitable<void> one(std::string value) = 0;
   virtual boost::asio::awaitable<void> many(std::uint32_t count, std::string value) = 0;
   virtual boost::asio::awaitable<void> fail() = 0;
   virtual boost::asio::awaitable<std::uint32_t> number(std::uint32_t value) = 0;
};

class void_service final : public void_api {
 public:
   boost::asio::awaitable<void> zero() override {
      calls.push_back("zero");
      co_return;
   }

   boost::asio::awaitable<void> one(std::string value) override {
      calls.push_back("one:" + value);
      co_return;
   }

   boost::asio::awaitable<void> many(std::uint32_t count, std::string value) override {
      calls.push_back("many:" + std::to_string(count) + ':' + value);
      co_return;
   }

   boost::asio::awaitable<void> fail() override {
      ++failures;
      throw core::exceptions::invocation_denied{"explicit void operation denial"};
      co_return;
   }

   boost::asio::awaitable<std::uint32_t> number(std::uint32_t value) override {
      co_return value + 1U;
   }

   std::vector<std::string> calls;
   std::size_t failures = 0;
};

FORGE_API(void_api, FORGE_API_CONTRACT("package.void", 1, 0), FORGE_API_METHOD(zero),
          FORGE_API_METHOD(one, value), FORGE_API_METHOD(many, count, value), FORGE_API_METHOD(fail),
          FORGE_API_METHOD(number, value))

namespace {

void require(bool condition, const char* message) {
   if (!condition) throw std::runtime_error{message};
}

template <typename Exception, typename Operation>
void rejects(forge::asio::runtime& runtime, Operation operation) {
   auto caught = false;
   try {
      forge::asio::blocking::run(runtime, operation());
   } catch (const Exception&) {
      caught = true;
   }
   require(caught, "public API call did not propagate the expected typed error");
}

// Only the in-memory transport boundary is supplied here. Dispatch, generated
// request decoding, invocation, empty response encoding and error projection
// remain the actual public Forge registry/frame_dispatcher implementation.
class wire_invoker final : public core::remote_invoker {
 public:
   explicit wire_invoker(core::frame_dispatcher& server) : _server(server) {}

   boost::asio::awaitable<core::response> async_call(core::request value) override {
      require(value.codec.value == "forge.raw", "wire call changed its public Raw codec");
      requests.push_back(value);
      const auto inbound = co_await _server.dispatch(core::frame{
          .kind = core::frame_kind::request, .id = {.value = ++_sequence}, .api = value.api,
          .method = value.method, .meta = std::move(value.meta), .codec = value.codec,
          .payload = std::move(value.body)});
      require(inbound.id.value == _sequence && inbound.api == value.api && inbound.method == value.method,
              "official dispatcher lost request/response correlation");
      auto result = core::response{.api = inbound.api, .method = inbound.method,
                                   .meta = inbound.meta, .codec = inbound.codec};
      if (inbound.kind == core::frame_kind::error) {
         result.error = core::unpack_body<core::error_payload>(inbound.payload);
      } else {
         require(inbound.kind == core::frame_kind::response, "official dispatcher returned no unary response");
         result.body = inbound.payload;
         response_bodies.push_back(result.body);
         if (nonempty_body) result.body.push_back(0xa5U);
      }
      co_return result;
   }

   bool nonempty_body = false;
   std::vector<core::request> requests;
   std::vector<core::bytes> response_bodies;

 private:
   core::frame_dispatcher& _server;
   std::uint64_t _sequence = 0;
};

// The typed invoker's public contract erases argument tuple and response types.
// This test transport adapts only its declared fixture tuple shapes to Raw,
// then uses the SAME real dispatcher above. There is no second method router.
class typed_invoker final : public core::remote_invoker {
 public:
   explicit typed_invoker(std::shared_ptr<wire_invoker> wire) : _wire(std::move(wire)) {}

   bool supports_typed_arguments() const noexcept override { return true; }

   boost::asio::awaitable<core::response> async_call(core::request) override {
      throw std::runtime_error{"generated typed proxy unexpectedly fell back to a raw call"};
      co_return core::response{};
   }

   boost::asio::awaitable<void> async_call_arguments(core::request value, std::type_index arguments_type,
                                                     void* arguments, std::type_index response_type,
                                                     void* response_storage) override {
      require(value.codec.value == "forge.typed" && arguments != nullptr,
              "typed proxy did not retain its real argument tuple");
      if (response_type == typeid(void)) {
         require(response_storage == nullptr, "void typed call allocated response storage");
         ++void_calls;
      } else {
         require(response_type == typeid(std::uint32_t) && response_storage != nullptr,
                 "non-void typed call lost its response storage");
      }
      if (arguments_type == typeid(std::tuple<>)) {
         value.body = core::pack_body(*static_cast<const std::tuple<>*>(arguments));
      } else if (arguments_type == typeid(std::tuple<std::string>)) {
         value.body = core::pack_body(std::get<0>(*static_cast<const std::tuple<std::string>*>(arguments)));
      } else if (arguments_type == typeid(std::tuple<std::uint32_t, std::string>)) {
         value.body = core::pack_body(*static_cast<const std::tuple<std::uint32_t, std::string>*>(arguments));
      } else if (arguments_type == typeid(std::tuple<std::uint32_t>)) {
         value.body = core::pack_body(std::get<0>(*static_cast<const std::tuple<std::uint32_t>*>(arguments)));
      } else {
         throw std::runtime_error{"typed proxy changed the declared argument tuple"};
      }
      value.codec.value = "forge.raw";
      const auto inbound = co_await _wire->async_call(std::move(value));
      if (inbound.error) {
         const auto descriptor = void_api::describe();
         core::raise_remote_error(*inbound.error, core::find_method(descriptor, inbound.method));
      }
      if (response_type == typeid(void)) {
         core::unpack_body<void>(inbound.body);
      } else if (!omit_number_response) {
         *static_cast<std::optional<std::uint32_t>*>(response_storage) = core::unpack_body<std::uint32_t>(inbound.body);
      }
      co_return;
   }

   std::size_t void_calls = 0;
   bool omit_number_response = false;

 private:
   std::shared_ptr<wire_invoker> _wire;
};

} // namespace

int main() {
   try {
      static_assert(std::same_as<decltype(std::declval<void_api&>().zero()), boost::asio::awaitable<void>>);
      const auto descriptor = void_api::describe();
      for (const auto* name : {"zero", "one", "many", "fail"}) {
         const auto* method = core::find_method(descriptor, name);
         require(method && method->response_type == typeid(void) && method->result_type == typeid(void),
                 "generated descriptor substituted another type for a void result");
      }
      auto runtime = forge::asio::runtime{{.worker_threads = 1}};
      auto implementation = std::make_shared<void_service>();
      auto registry = core::registry{};
      registry.install<void_api>(implementation);
      auto server = core::frame_dispatcher{core::binding().serve(registry).build()};
      auto wire = std::make_shared<wire_invoker>(server);
      auto raw = core::handle<void_api>{std::make_shared<core::proxy<void_api>>(wire)};
      forge::asio::blocking::run(runtime, raw->zero());
      forge::asio::blocking::run(runtime, raw->one("raw"));
      forge::asio::blocking::run(runtime, raw->many(7, "raw"));
      require(implementation->calls == std::vector<std::string>{"zero", "one:raw", "many:7:raw"},
              "raw generated void proxy did not invoke the exact zero/one/multiple arguments");
      require(wire->requests.front().body.empty(), "zero-argument API request acquired synthetic wire bytes");
      for (const auto& body : wire->response_bodies) require(body.empty(), "actual void dispatcher response contains a body");

      // Both direct remote call entrypoints must compile/execute void without
      // attempting to instantiate Raw<void> or optional<void>.
      forge::asio::blocking::run(runtime, wire->call<std::tuple<>, void>(descriptor, void_api::ref(), "zero", {}));
      forge::asio::blocking::run(runtime, wire->call<std::string, void>(descriptor, void_api::ref(), "one", "direct"));
      forge::asio::blocking::run(runtime, wire->call_arguments<void>(descriptor, void_api::ref(), "many", std::uint32_t{9}, std::string{"direct"}));
      require(implementation->calls.size() == 6 && implementation->calls.back() == "many:9:direct",
              "direct remote void entrypoint lost a successful invocation");

      auto typed_transport = std::make_shared<typed_invoker>(wire);
      auto typed = core::handle<void_api>{std::make_shared<core::proxy<void_api>>(typed_transport)};
      forge::asio::blocking::run(runtime, typed->zero());
      forge::asio::blocking::run(runtime, typed->one("typed"));
      forge::asio::blocking::run(runtime, typed->many(11, "typed"));
      require(typed_transport->void_calls == 3 && implementation->calls.size() == 9 &&
                  implementation->calls[7] == "one:typed" && implementation->calls[8] == "many:11:typed",
              "typed void proxy did not dispatch exact arguments with null response storage");
      forge::asio::blocking::run(runtime, registry.get<void_api>(void_api::ref())->one("local"));
      require(implementation->calls.back() == "one:local", "direct local void handle regressed");

      rejects<core::exceptions::invocation_denied>(runtime, [&] { return raw->fail(); });
      rejects<core::exceptions::invocation_denied>(runtime, [&] { return typed->fail(); });
      rejects<core::exceptions::invocation_denied>(runtime, [&] {
         return wire->call<std::tuple<>, void>(descriptor, void_api::ref(), "fail", {});
      });
      require(implementation->failures == 3, "void remote errors bypassed or duplicated the actual server call");

      wire->nonempty_body = true;
      rejects<core::exceptions::protocol_error>(runtime, [&] { return raw->zero(); });
      rejects<core::exceptions::protocol_error>(runtime, [&] { return typed->one("nonempty"); });
      rejects<core::exceptions::protocol_error>(runtime, [&] {
         return wire->call<std::string, void>(descriptor, void_api::ref(), "one", "nonempty-direct");
      });
      wire->nonempty_body = false;
      const auto empty = core::bytes{};
      core::unpack_body<void>(empty);
      core::unpack_body<void>(std::span<const std::uint8_t>{empty});
      auto nonempty_rejected = false;
      try { core::unpack_body<void>(core::bytes{0}); }
      catch (const core::exceptions::protocol_error&) { nonempty_rejected = true; }
      require(nonempty_rejected, "void body decoder accepted an unrelated byte");

      require(forge::asio::blocking::run(runtime, raw->number(41)) == 42 &&
                  forge::asio::blocking::run(runtime, typed->number(42)) == 43,
              "void handling changed an ordinary typed response");
      typed_transport->omit_number_response = true;
      rejects<core::exceptions::protocol_error>(runtime, [&] { return typed->number(1); });
      std::cout << "PASS: public API void responses; real dispatcher, raw/typed generated proxies, errors and empty-body checks\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "Void response package consumer failed: " << error.what() << '\n';
      return 1;
   }
}
