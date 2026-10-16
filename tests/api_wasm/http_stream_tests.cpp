#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <string>

import forge.api.wasm.http_stream;
import forge.api.wasm.codec;
import forge.raw.exceptions;

namespace {
namespace protocol = forge::api::wasm;
namespace errors = protocol::exceptions;

template <typename T> void golden(const T& value, const forge::raw::bytes& bytes) {
   BOOST_CHECK(forge::raw::pack(value) == bytes);
   BOOST_CHECK(forge::raw::unpack_exact<T>(bytes) == value);
   BOOST_CHECK(protocol::decode<T>(bytes) == value);
}

protocol::http_stream_open opening() {
   return {{forge::net::http::method::get, "/x", {}}, "item", "END"};
}
} // namespace

BOOST_AUTO_TEST_SUITE(wasm_http_stream)

BOOST_AUTO_TEST_CASE(records_preserve_literal_wire_layout) {
   golden(opening(), {2, 0, 0, 0, 2, '/', 'x', 0, 4, 'i', 't', 'e', 'm', 3, 'E', 'N', 'D'});
   golden(protocol::http_stream_read{}, {0, 0});
   golden(protocol::http_stream_read{protocol::payload{"item", {1, 2}}, true}, {1, 4, 'i', 't', 'e', 'm', 2, 1, 2, 1});
   golden(protocol::http_stream_event{200, false, protocol::payload{"item", {1, 2}}},
          {0xc8, 0, 0, 1, 4, 'i', 't', 'e', 'm', 2, 1, 2});
   golden(protocol::http_stream_event{200, true, {}}, {0xc8, 0, 1, 0});
   BOOST_TEST(protocol::http_stream_feature == "http.stream.v1");
}

BOOST_AUTO_TEST_CASE(open_reuses_http_bounds_and_validates_framing) {
   BOOST_CHECK_NO_THROW(protocol::validate(opening()));
   auto value = opening();
   value.request.method = forge::net::http::method::patch;
   BOOST_CHECK_NO_THROW(protocol::validate(value));
   value.request.target = "//other.test/x";
   BOOST_CHECK_THROW(protocol::validate(value), errors::invalid_value);
   value = opening();
   value.request.target = std::string{"/x\0hidden", 9};
   BOOST_CHECK_THROW(protocol::validate(value), errors::invalid_value);
   value = opening();
   value.event_type = "bad type";
   BOOST_CHECK_THROW(protocol::validate(value), errors::invalid_value);
   value = opening();
   value.end_marker.clear();
   BOOST_CHECK_THROW(protocol::validate(value), errors::invalid_value);
   value.end_marker = "bad\nmarker";
   BOOST_CHECK_THROW(protocol::validate(value), errors::invalid_value);
   value.end_marker = std::string(64, 'x');
   BOOST_CHECK_NO_THROW(protocol::validate(value));
   value.end_marker.push_back('x');
   BOOST_CHECK_THROW(protocol::validate(value), errors::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(read_and_event_enforce_payload_and_aggregate_bounds) {
   const auto read = protocol::http_stream_read{protocol::payload{"item", {1, 2}}, true};
   auto bounds = protocol::limits{};
   bounds.payload_bytes = 2;
   BOOST_CHECK_NO_THROW(protocol::validate(read, bounds));
   bounds.payload_bytes = 1;
   BOOST_CHECK_THROW(protocol::validate(read, bounds), errors::limit_exceeded);
   bounds = {};
   bounds.wire.max_bytes = static_cast<std::uint32_t>(forge::raw::pack_size(read) - 1U);
   BOOST_CHECK_THROW(protocol::validate(read, bounds), errors::limit_exceeded);
   const auto event = protocol::http_stream_event{200, false, read.emission};
   bounds = {};
   bounds.payload_bytes = 1;
   BOOST_CHECK_THROW(protocol::validate(event, bounds), errors::limit_exceeded);
   bounds = {};
   bounds.wire.max_bytes = static_cast<std::uint32_t>(forge::raw::pack_size(event) - 1U);
   BOOST_CHECK_THROW(protocol::validate(event, bounds), errors::limit_exceeded);
   auto open = opening();
   open.request.body = read.emission;
   bounds = {};
   bounds.payload_bytes = 1;
   BOOST_CHECK_THROW(protocol::validate(open, bounds), errors::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(event_status_and_pinned_item_type_are_checked_without_terminal_inference) {
   auto event = protocol::http_stream_event{200, false, protocol::payload{"item", {1}}};
   BOOST_CHECK_NO_THROW(protocol::validate(event, "item"));
   BOOST_CHECK_THROW(protocol::validate(event, "other"), errors::wrong_type);
   event.ended = true;
   BOOST_CHECK_NO_THROW(protocol::validate(event));
   event.item.reset();
   BOOST_CHECK_NO_THROW(protocol::validate(event));
   event.ended = false;
   BOOST_CHECK_NO_THROW(protocol::validate(event));
   event.status = 99;
   BOOST_CHECK_THROW(protocol::validate(event), errors::invalid_http_response);
   event.status = 600;
   BOOST_CHECK_THROW(protocol::validate(event), errors::invalid_http_response);
   event.status = 599;
   BOOST_CHECK_NO_THROW(protocol::validate(event));
}

BOOST_AUTO_TEST_CASE(wire_decode_rejects_truncation_and_validates_values) {
   auto bytes = forge::raw::pack(opening());
   bytes.pop_back();
   BOOST_CHECK_THROW(protocol::decode<protocol::http_stream_open>(bytes), forge::raw::exceptions::range_error);
   const auto invalid = forge::raw::pack(protocol::http_stream_event{0, false, {}});
   BOOST_CHECK_THROW(protocol::decode<protocol::http_stream_event>(invalid), errors::invalid_http_response);
}

BOOST_AUTO_TEST_SUITE_END()
