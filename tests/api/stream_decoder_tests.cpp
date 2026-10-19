#include <boost/asio/awaitable.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>

#include <cstdint>

import forge.api.core.connection;
import forge.api.core.duplex_stream;
import forge.api.core.stream_reader;
import forge.api.core.stream_writer;
import forge.raw.raw;
import forge.raw.exceptions;

namespace stream_decoder_test {

class api
    : public forge::api::core::contract<api, forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<std::uint32_t> echo(std::uint32_t value) = 0;
   virtual boost::asio::awaitable<void> download(forge::api::core::stream_writer<std::uint32_t>) = 0;
   virtual boost::asio::awaitable<void> upload(forge::api::core::stream_reader<std::uint32_t>) = 0;
   virtual boost::asio::awaitable<void> exchange(forge::api::core::duplex_stream<std::uint32_t, std::uint32_t>) = 0;
};

} // namespace stream_decoder_test

FORGE_API(::stream_decoder_test::api, FORGE_API_CONTRACT("test.stream.decoder", 1, 0), FORGE_API_METHOD(echo),
          FORGE_API_METHOD(download), FORGE_API_METHOD(upload), FORGE_API_METHOD(exchange))

BOOST_AUTO_TEST_SUITE(api_stream_decoders)

BOOST_AUTO_TEST_CASE(custom_decoders_receive_limits_and_enforce_semantics) {
   namespace core = forge::api::core;
   using api = stream_decoder_test::api;
   auto builder = core::define<api>(core::descriptor{.id = {"test.custom.codec"}, .version = {1, 0}});
   auto observed = std::uint32_t{};
   auto calls = 0U;
   auto method = builder.method<&api::exchange>("exchange");
   method.input_decoder([&](const core::bytes& bytes, forge::raw::unpack_limits limits) {
      observed = limits.max_bytes;
      ++calls;
      const auto value = forge::raw::unpack_exact<std::uint32_t>(bytes, limits);
      if (value > 7U)
         throw core::exceptions::protocol_error{"input exceeds semantic bound"};
   });
   method.output_decoder([&](const core::bytes& bytes, forge::raw::unpack_limits limits) {
      observed = limits.max_bytes;
      ++calls;
      const auto value = forge::raw::unpack_exact<std::uint32_t>(bytes, limits);
      if (value == 0U)
         throw core::exceptions::protocol_error{"zero output is unavailable"};
   });
   const auto descriptor = builder.build();
   const auto& stream = descriptor.methods.at(0);
   const auto limits = forge::raw::unpack_limits{.max_bytes = 4U};
   stream.input_decoder(forge::raw::pack(std::uint32_t{7}), limits);
   BOOST_CHECK_THROW(stream.input_decoder(forge::raw::pack(std::uint32_t{8}), limits),
                     core::exceptions::protocol_error);
   BOOST_CHECK_THROW(stream.output_decoder(forge::raw::pack(std::uint32_t{0}), limits),
                     core::exceptions::protocol_error);
   stream.output_decoder(forge::raw::pack(std::uint32_t{1}), limits);
   BOOST_TEST(calls == 4U);
   BOOST_TEST(observed == 4U);
}

BOOST_AUTO_TEST_CASE(wrong_directions_and_empty_decoders_are_rejected_without_erasing_defaults) {
   namespace core = forge::api::core;
   using api = stream_decoder_test::api;
   auto builder = core::define<api>(core::descriptor{.id = {"test.codec.directions"}, .version = {1, 0}});
   auto unary = builder.method<&api::echo>("echo");
   const auto decoder = [](const core::bytes&, forge::raw::unpack_limits) {};
   BOOST_CHECK_THROW(unary.input_decoder(decoder), core::exceptions::protocol_error);
   BOOST_CHECK_THROW(unary.output_decoder(decoder), core::exceptions::protocol_error);
   auto download = builder.method<&api::download>("download");
   BOOST_CHECK_THROW(download.input_decoder(decoder), core::exceptions::protocol_error);
   BOOST_CHECK_THROW(download.output_decoder({}), core::exceptions::protocol_error);
   auto upload = builder.method<&api::upload>("upload");
   BOOST_CHECK_THROW(upload.output_decoder(decoder), core::exceptions::protocol_error);
   BOOST_CHECK_THROW(upload.input_decoder({}), core::exceptions::protocol_error);
   const auto descriptor = builder.build();
   BOOST_REQUIRE(descriptor.methods[1].output_decoder);
   BOOST_REQUIRE(descriptor.methods[2].input_decoder);
   const auto malformed = core::bytes{1};
   BOOST_CHECK_THROW(descriptor.methods[1].output_decoder(malformed, {}), forge::raw::exceptions::range_error);
   BOOST_CHECK_THROW(descriptor.methods[2].input_decoder(malformed, {}), forge::raw::exceptions::range_error);
}

BOOST_AUTO_TEST_CASE(local_descriptors_cannot_install_unused_wire_decoders) {
   namespace core = forge::api::core;
   using api = stream_decoder_test::api;
   auto builder = core::define<api, false>(core::descriptor{.id = {"test.local.codec"}, .version = {1, 0}});
   auto method = builder.method<&api::exchange>("exchange");
   const auto decoder = [](const core::bytes&, forge::raw::unpack_limits) {};
   BOOST_CHECK_THROW(method.input_decoder(decoder), core::exceptions::protocol_error);
   BOOST_CHECK_THROW(method.output_decoder(decoder), core::exceptions::protocol_error);
}

BOOST_AUTO_TEST_SUITE_END()
