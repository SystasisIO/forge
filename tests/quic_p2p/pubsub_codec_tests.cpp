#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

import forge.net.p2p.exceptions;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;

namespace {

namespace ps = forge::net::p2p::pubsub;
using bytes = std::vector<std::uint8_t>;
using codec_error = forge::net::p2p::exceptions::codec_error;
using invalid_options = forge::net::p2p::exceptions::invalid_options;

// Small independent protobuf fixtures; neither helper calls the production codec.
bytes framed(bytes payload) {
   BOOST_REQUIRE_LT(payload.size(), 128U);
   payload.insert(payload.begin(), static_cast<std::uint8_t>(payload.size()));
   return payload;
}

bytes field(std::uint8_t key, bytes payload) {
   auto out = framed(std::move(payload));
   out.insert(out.begin(), key);
   return out;
}

void check_bytes(const bytes& actual, const bytes& expected) {
   BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
}

} // namespace

BOOST_AUTO_TEST_SUITE(pubsub_codec_tests)

BOOST_AUTO_TEST_CASE(legacy_goldens_and_protocol_identifiers_keep_default_v11) {
   static_assert(static_cast<unsigned>(ps::version::v1_0) == 0);
   static_assert(static_cast<unsigned>(ps::version::v1_1) == 1);
   BOOST_CHECK(ps::options{}.preferred == ps::version::v1_1);
   BOOST_CHECK_EQUAL(ps::codec::protocol(ps::version::v1_0).value, "/meshsub/1.0.0");
   BOOST_CHECK_EQUAL(ps::codec::protocol(ps::version::v1_1).value, "/meshsub/1.1.0");
   BOOST_CHECK_EQUAL(ps::codec::protocol(ps::version::v1_2).value, "/meshsub/1.2.0");
   BOOST_CHECK_EQUAL(ps::codec::protocol(ps::version::v1_3).value, "/meshsub/1.3.0");

   auto rpc = ps::rpc{};
   rpc.subscriptions.push_back({.subscribe = true, .subject = {.value = "t"}});
   rpc.control_value.emplace();
   rpc.control_value->prunes.push_back({.subject = {.value = "t"}, .backoff = std::chrono::seconds{5}});
   const auto v10 = bytes{0x0e, 0x0a, 5, 8, 1, 0x12, 1, 't', 0x1a, 5, 0x22, 3, 0x0a, 1, 't'};
   const auto v11 = bytes{0x10, 0x0a, 5, 8, 1, 0x12, 1, 't', 0x1a, 7, 0x22, 5, 0x0a, 1, 't', 0x18, 5};
   auto opts = ps::options{};
   opts.preferred = ps::version::v1_0;
   check_bytes(ps::codec::encode(rpc, opts), v10);
   check_bytes(ps::codec::encode(ps::codec::decode(v10, opts), opts), v10);
   for (const auto version : {ps::version::v1_1, ps::version::v1_2, ps::version::v1_3}) {
      opts.preferred = version;
      check_bytes(ps::codec::encode(rpc, opts), v11);
      check_bytes(ps::codec::encode(ps::codec::decode(v11, opts), opts), v11);
   }
}

BOOST_AUTO_TEST_CASE(idontwant_field_five_preserves_binary_and_empty_ids) {
   // ControlIDontWant.messageIDs is bytes on the wire, not validated UTF-8.
   const auto golden = bytes{0x0b, 0x1a, 9, 0x2a, 7, 0x0a, 3, 0xff, 0, 0x80, 0x0a, 0};
   auto opts = ps::options{};
   opts.preferred = ps::version::v1_2;
   const auto rpc = ps::codec::decode(golden, opts);
   BOOST_REQUIRE(rpc.control_value);
   BOOST_REQUIRE_EQUAL(rpc.control_value->dont_want.size(), 1U);
   const auto& ids = rpc.control_value->dont_want.front().message_ids;
   BOOST_REQUIRE_EQUAL(ids.size(), 2U);
   check_bytes(ids[0], {0xff, 0, 0x80});
   BOOST_CHECK(ids[1].empty());
   check_bytes(ps::codec::encode(rpc, opts), golden);
   // Decoding wire values is independent of negotiated stream capabilities.
   opts.preferred = ps::version::v1_0;
   BOOST_REQUIRE_EQUAL(ps::codec::decode(golden, opts).control_value->dont_want.size(), 1U);
}

BOOST_AUTO_TEST_CASE(v13_advertisement_and_subscription_flags_preserve_presence) {
   const auto golden = bytes{0x11, 0x0a, 9, 8, 1, 0x12, 1, 't', 0x18, 0, 0x20, 1,
                              0x1a, 4, 0x32, 2, 0x50, 1};
   const auto rpc = ps::codec::decode(golden, ps::options{});
   BOOST_REQUIRE_EQUAL(rpc.subscriptions.size(), 1U);
   const auto& sub = rpc.subscriptions.front();
   BOOST_REQUIRE(sub.requests_partial.has_value());
   BOOST_CHECK(!*sub.requests_partial);
   BOOST_REQUIRE(sub.supports_sending_partial.has_value());
   BOOST_CHECK(*sub.supports_sending_partial);
   BOOST_REQUIRE(rpc.control_value && rpc.control_value->extensions);
   BOOST_CHECK(rpc.control_value->extensions->partial_messages == std::optional<bool>{true});
   check_bytes(ps::codec::encode(rpc), golden);

   for (const bool subscribe : {false, true}) {
      auto value = rpc;
      value.subscriptions.front().subscribe = subscribe;
      value.subscriptions.front().requests_partial = true;
      value.subscriptions.front().supports_sending_partial = false;
      value.control_value->extensions->partial_messages = false;
      const auto decoded = ps::codec::decode(ps::codec::encode(value), ps::options{});
      BOOST_CHECK_EQUAL(decoded.subscriptions.front().subscribe, subscribe);
      BOOST_CHECK(decoded.subscriptions.front().requests_partial == std::optional<bool>{true});
      BOOST_CHECK(decoded.subscriptions.front().supports_sending_partial == std::optional<bool>{false});
      BOOST_CHECK(decoded.control_value->extensions->partial_messages == std::optional<bool>{false});
   }
   const auto empty = ps::codec::decode(bytes{4, 0x1a, 2, 0x32, 0}, ps::options{});
   BOOST_REQUIRE(empty.control_value && empty.control_value->extensions);
   BOOST_CHECK(!empty.control_value->extensions->partial_messages);
   check_bytes(ps::codec::encode(empty), {4, 0x1a, 2, 0x32, 0});
   BOOST_CHECK(!ps::codec::decode(bytes{0}, ps::options{}).control_value);
}

BOOST_AUTO_TEST_CASE(partial_field_ten_has_independent_optional_binary_fields) {
   const auto golden = bytes{0x0e, 0x52, 0x0c, 0x0a, 1, 't', 0x12, 2, 0, 0xff, 0x1a, 0, 0x22, 1, 0x80};
   const auto rpc = ps::codec::decode(golden, ps::options{});
   BOOST_REQUIRE(rpc.partial);
   BOOST_REQUIRE(rpc.partial->subject);
   BOOST_CHECK_EQUAL(rpc.partial->subject->value, "t");
   BOOST_REQUIRE(rpc.partial->group_id);
   check_bytes(*rpc.partial->group_id, {0, 0xff});
   BOOST_REQUIRE(rpc.partial->data);
   BOOST_CHECK(rpc.partial->data->empty());
   BOOST_REQUIRE(rpc.partial->metadata);
   check_bytes(*rpc.partial->metadata, {0x80});
   BOOST_CHECK(rpc.messages.empty() && !rpc.control_value);
   check_bytes(ps::codec::encode(rpc), golden);
   const auto received = ps::codec::decode_received(golden, ps::options{});
   BOOST_CHECK(received.invalid_messages.empty());
   BOOST_REQUIRE(received.value.partial);

   const auto absent = ps::codec::decode(bytes{2, 0x52, 0}, ps::options{});
   BOOST_REQUIRE(absent.partial);
   BOOST_CHECK(!absent.partial->subject && !absent.partial->group_id &&
               !absent.partial->data && !absent.partial->metadata);
   check_bytes(ps::codec::encode(absent), {2, 0x52, 0});
   const auto empty = ps::codec::decode(bytes{10, 0x52, 8, 0x0a, 0, 0x12, 0, 0x1a, 0, 0x22, 0}, ps::options{});
   BOOST_REQUIRE(empty.partial && empty.partial->subject && empty.partial->group_id &&
                 empty.partial->data && empty.partial->metadata);
   BOOST_CHECK(empty.partial->subject->value.empty() && empty.partial->group_id->empty() &&
               empty.partial->data->empty() && empty.partial->metadata->empty());
   check_bytes(ps::codec::encode(empty), {10, 0x52, 8, 0x0a, 0, 0x12, 0, 0x1a, 0, 0x22, 0});

   auto binary_topic = rpc;
   binary_topic.partial->subject->value = std::string{"\xff\0", 2};
   BOOST_CHECK(ps::codec::decode(ps::codec::encode(binary_topic), ps::options{}).partial->subject->value ==
               binary_topic.partial->subject->value);
}

BOOST_AUTO_TEST_CASE(unknown_extensions_and_control_fields_do_not_become_capabilities) {
   const auto extension = bytes{0x58, 1, 0x61, 1, 2, 3, 4, 5, 6, 7, 8, 0x6a, 2, 0xff, 0};
   auto control = bytes{0x38, 1}; // Unknown Control field 7 with a non-message wire type.
   const auto advertisement = field(0x32, extension);
   control.insert(control.end(), advertisement.begin(), advertisement.end());
   const auto rpc = ps::codec::decode(framed(field(0x1a, control)), ps::options{});
   BOOST_REQUIRE(rpc.control_value && rpc.control_value->extensions);
   BOOST_CHECK(!rpc.control_value->extensions->partial_messages);
   check_bytes(ps::codec::encode(rpc), {4, 0x1a, 2, 0x32, 0});

   // Unknown fields can coexist with known false and never advertise true.
   auto known = extension;
   known.insert(known.end(), {0x50, 0});
   const auto decoded = ps::codec::decode(framed(field(0x1a, field(0x32, known))), ps::options{});
   BOOST_CHECK(decoded.control_value->extensions->partial_messages == std::optional<bool>{false});
   const auto partial = ps::codec::decode(framed(field(0x52, {0x28, 1, 0x0a, 1, 't'})), ps::options{});
   BOOST_REQUIRE(partial.partial && partial.partial->subject);
   BOOST_CHECK_EQUAL(partial.partial->subject->value, "t");
}

BOOST_AUTO_TEST_CASE(unknown_fixed32_and_bytes_skip_exactly_to_the_next_field) {
   // The unknown bytes contain keys which must not be interpreted as nested extensions.
   const auto extension = bytes{0x5d, 0, 0xff, 0x80, 1, 0x62, 4, 0x50, 1, 0, 0xff, 0x50, 0};
   auto control = bytes{0x3d, 1, 2, 3, 4};
   const auto advertisement = field(0x32, extension);
   control.insert(control.end(), advertisement.begin(), advertisement.end());
   auto payload = bytes{0x5d, 5, 6, 7, 8};
   const auto nested = field(0x1a, control);
   payload.insert(payload.end(), nested.begin(), nested.end());
   const auto rpc = ps::codec::decode(framed(payload), ps::options{});
   BOOST_REQUIRE(rpc.control_value && rpc.control_value->extensions);
   BOOST_CHECK(rpc.control_value->extensions->partial_messages == std::optional<bool>{false});
   check_bytes(ps::codec::encode(rpc), {6, 0x1a, 4, 0x32, 2, 0x50, 0});

   // Largest legal 29-bit field number, with fixed32 data, is still an unknown field.
   const auto maximum = framed(field(0x1a, field(0x32, {0xfd, 0xff, 0xff, 0xff, 0x0f, 1, 2, 3, 4,
                                                       0x50, 1})));
   BOOST_CHECK(ps::codec::decode(maximum, ps::options{}).control_value->extensions->partial_messages ==
               std::optional<bool>{true});
}

BOOST_AUTO_TEST_CASE(unknown_fields_reject_truncation_and_invalid_numbers_before_narrowing) {
   auto malformed = std::vector<bytes>{
       {0x62, 4, 1, 2, 3},                       // Unknown bytes are truncated.
       {0x62, 0x80},                             // Truncated unknown length varint.
       {0, 1}, {2, 0}, {5, 1, 2, 3, 4},         // Field zero for supported wire types.
       {0x80, 0x80, 0x80, 0x80, 0x10, 1},       // Field 2^29 exceeds the protobuf range.
       {0xd0, 0x80, 0x80, 0x80, 0x80, 1, 1},   // Field 2^32+10 must not alias partialMessages.
       {0x51, 1, 2, 3, 4, 5, 6, 7, 8},         // Known bool still rejects fixed64.
       {0x55, 1, 2, 3, 4},                      // Known bool still rejects fixed32.
   };
   for (std::size_t size = 0; size < 4; ++size) {
      auto truncated = bytes{0x5d};
      truncated.insert(truncated.end(), size, 0);
      malformed.push_back(std::move(truncated));
   }
   for (const auto& extension : malformed) {
      const auto wire = framed(field(0x1a, field(0x32, extension)));
      BOOST_CHECK_THROW((void)ps::codec::decode(wire, ps::options{}), codec_error);
      BOOST_CHECK_THROW((void)ps::codec::decode_received(wire, ps::options{}), codec_error);
   }
   // Reject the same invalid field-number encodings at the outer RPC boundary too.
   for (const auto& key : std::vector<bytes>{{0, 1}, {0x80, 0x80, 0x80, 0x80, 0x10, 1},
                                            {0xd0, 0x80, 0x80, 0x80, 0x80, 1, 1}}) {
      BOOST_CHECK_THROW((void)ps::codec::decode(framed(key), ps::options{}), codec_error);
   }
}

BOOST_AUTO_TEST_CASE(singular_messages_merge_and_scalar_bytes_last_value_wins) {
   const auto wire = framed({0x1a, 7, 0x2a, 3, 0x0a, 1, 'a', 0x32, 0,
                             0x1a, 11, 0x2a, 3, 0x0a, 1, 'b', 0x32, 4, 0x50, 1, 0x50, 0,
                             0x52, 6, 0x0a, 1, 't', 0x12, 1, 'g',
                             0x52, 7, 0x1a, 1, 'a', 0x1a, 0, 0x22, 0});
   const auto rpc = ps::codec::decode(wire, ps::options{});
   BOOST_REQUIRE(rpc.control_value && rpc.control_value->extensions);
   BOOST_REQUIRE_EQUAL(rpc.control_value->dont_want.size(), 2U);
   check_bytes(rpc.control_value->dont_want[0].message_ids[0], {'a'});
   check_bytes(rpc.control_value->dont_want[1].message_ids[0], {'b'});
   BOOST_CHECK(rpc.control_value->extensions->partial_messages == std::optional<bool>{false});
   BOOST_REQUIRE(rpc.partial && rpc.partial->subject && rpc.partial->group_id &&
                 rpc.partial->data && rpc.partial->metadata);
   BOOST_CHECK_EQUAL(rpc.partial->subject->value, "t");
   check_bytes(*rpc.partial->group_id, {'g'});
   BOOST_CHECK(rpc.partial->data->empty() && rpc.partial->metadata->empty());
   auto opts = ps::options{};
   opts.limits.max_message_ids = 1;
   BOOST_CHECK_THROW((void)ps::codec::decode(wire, opts), codec_error);
   opts.limits.max_message_ids = 2;
   opts.limits.max_control_entries = 3; // Two IDONTWANT and two advertisement occurrences.
   BOOST_CHECK_THROW((void)ps::codec::decode(wire, opts), codec_error);
}

BOOST_AUTO_TEST_CASE(new_fields_reject_wrong_types_and_truncated_payloads) {
   const auto malformed = std::vector<bytes>{
       {0x1a, 2, 0x28, 0},                         // IDONTWANT must be a message.
       {0x1a, 4, 0x2a, 2, 8, 1},                  // Message ID must be bytes.
       {0x1a, 4, 0x32, 2, 0x52, 0},               // partialMessages must be varint.
       {0x52, 2, 8, 1},                           // Partial.topicID must be bytes.
       {0x50, 1},                                 // Partial must be a message.
       {0x0a, 7, 8, 1, 0x12, 1, 't', 0x1a, 0},   // requestsPartial must be varint.
       {0x0a, 7, 8, 1, 0x12, 1, 't', 0x22, 0},   // supportsSendingPartial must be varint.
       {0x1a, 5, 0x2a, 3, 0x0a, 4, 0xff},         // Truncated ID.
       {0x52, 3, 0x1a, 4, 1},                     // Truncated partial body.
       {0x1a, 3, 0x32, 1, 0x50},                  // Missing bool value.
   };
   for (const auto& payload : malformed) {
      BOOST_CHECK_THROW((void)ps::codec::decode(framed(payload), ps::options{}), codec_error);
      BOOST_CHECK_THROW((void)ps::codec::decode_received(framed(payload), ps::options{}), codec_error);
   }
}

BOOST_AUTO_TEST_CASE(idontwant_limits_are_aggregate_and_cover_encode_and_decode) {
   const auto wire = framed({0x1a, 11, 0x2a, 4, 0x0a, 2, 'a', 'b', 0x2a, 3, 0x0a, 1, 'c'});
   const auto rpc = ps::codec::decode(wire, ps::options{});
   for (int bound = 0; bound != 4; ++bound) {
      auto opts = ps::options{};
      if (bound == 0) { opts.limits.max_message_ids = 1; }
      if (bound == 1) { opts.limits.max_idontwant_message_id_size = 1; }
      if (bound == 2) { opts.limits.max_control_entries = 1; }
      if (bound == 3) { opts.limits.max_rpc_size = wire.size() - 2; }
      BOOST_CHECK_THROW((void)ps::codec::encode(rpc, opts), invalid_options);
      BOOST_CHECK_THROW((void)ps::codec::decode(wire, opts), codec_error);
   }
   auto opts = ps::options{};
   opts.limits.max_message_ids = 2;
   opts.limits.max_idontwant_message_id_size = 2;
   opts.limits.max_control_entries = 2;
   opts.limits.max_rpc_size = wire.size() - 1;
   check_bytes(ps::codec::encode(rpc, opts), wire);
   BOOST_CHECK_NO_THROW((void)ps::codec::decode(wire, opts));
   auto advertisement = rpc;
   advertisement.control_value->extensions.emplace();
   BOOST_CHECK_THROW((void)ps::codec::encode(advertisement, opts), invalid_options);
}

BOOST_AUTO_TEST_CASE(partial_field_and_aggregate_limits_cannot_be_bypassed_by_merging) {
   const auto wire = framed(field(0x52, {0x0a, 2, 't', 't', 0x12, 2, 0, 1, 0x1a, 2, 2, 3, 0x22, 2, 4, 5}));
   const auto rpc = ps::codec::decode(wire, ps::options{});
   for (int bound = 0; bound != 6; ++bound) {
      auto opts = ps::options{};
      if (bound == 0) { opts.limits.max_topic_size = 1; }
      if (bound == 1) { opts.limits.max_partial_group_id_size = 1; }
      if (bound == 2) { opts.limits.max_data_size = 1; }
      if (bound == 3) { opts.limits.max_partial_metadata_size = 1; }
      if (bound == 4) { opts.limits.max_message_size = 15; }
      if (bound == 5) { opts.limits.max_rpc_size = wire.size() - 2; }
      BOOST_CHECK_THROW((void)ps::codec::encode(rpc, opts), invalid_options);
      BOOST_CHECK_THROW((void)ps::codec::decode(wire, opts), codec_error);
   }
   auto opts = ps::options{};
   opts.limits.max_message_size = 16;
   check_bytes(ps::codec::encode(rpc, opts), wire);
   BOOST_CHECK_NO_THROW((void)ps::codec::decode(wire, opts));
   opts.limits.max_message_size = 3;
   // Each occurrence fits by itself, but the merged Partial does not.
   BOOST_CHECK_THROW((void)ps::codec::decode(framed({0x52, 3, 0x0a, 1, 't', 0x52, 3, 0x12, 1, 'g'}), opts),
                     codec_error);
}

BOOST_AUTO_TEST_CASE(partial_rpc_preflight_counts_length_boundaries_and_existing_fields) {
   // Independent wire sizes include the data field, Partial key and Partial length, not the RPC prefix.
   const auto sizes = std::vector<std::pair<std::size_t, std::size_t>>{
       {0, 4}, {60, 64}, {125, 129}, {126, 131}, {127, 132}, {128, 134},
       {16380, 16386}, {16381, 16388}};
   for (const auto& [body_size, partial_size] : sizes) {
      for (int fields = 0; fields != 8; ++fields) {
         auto rpc = ps::rpc{};
         auto prefix_size = std::size_t{};
         if ((fields & 1) != 0) {
            rpc.subscriptions.push_back({.subject = {.value = "t"}});
            prefix_size += 7;
         }
         if ((fields & 2) != 0) {
            rpc.control_value.emplace();
            rpc.control_value->grafts.push_back({.subject = {.value = "t"}});
            prefix_size += 7;
         }
         if ((fields & 4) != 0) {
            rpc.messages.push_back({.subject = {.value = "t"}});
            prefix_size += 5;
         }
         rpc.partial.emplace();
         rpc.partial->data.emplace(body_size, 0x5a);
         auto opts = ps::options{};
         opts.limits.max_rpc_size = prefix_size + partial_size;
         const auto encoded = ps::codec::encode(rpc, opts);
         const auto framing_size = opts.limits.max_rpc_size < 128 ? 1 :
             opts.limits.max_rpc_size < 16384 ? 2 : 3;
         BOOST_CHECK_EQUAL(encoded.size(), opts.limits.max_rpc_size + framing_size);
         const auto decoded = ps::codec::decode(encoded, opts);
         BOOST_REQUIRE(decoded.partial && decoded.partial->data);
         check_bytes(*decoded.partial->data, *rpc.partial->data);
         --opts.limits.max_rpc_size;
         BOOST_CHECK_THROW((void)ps::codec::encode(rpc, opts), invalid_options);
         BOOST_CHECK_THROW((void)ps::codec::decode(encoded, opts), codec_error);
      }
   }
   auto empty = ps::rpc{};
   empty.partial.emplace();
   auto opts = ps::options{};
   opts.limits.max_rpc_size = 2;
   check_bytes(ps::codec::encode(empty, opts), {2, 0x52, 0});
   opts.limits.max_rpc_size = 1;
   BOOST_CHECK_THROW((void)ps::codec::encode(empty, opts), invalid_options);
}

BOOST_AUTO_TEST_CASE(new_bounds_must_be_positive_and_partial_is_not_a_signed_publish) {
   for (int bound = 0; bound != 3; ++bound) {
      auto opts = ps::options{};
      if (bound == 0) { opts.limits.max_idontwant_message_id_size = 0; }
      if (bound == 1) { opts.limits.max_partial_group_id_size = 0; }
      if (bound == 2) { opts.limits.max_partial_metadata_size = 0; }
      BOOST_CHECK_THROW((void)ps::codec::encode(ps::rpc{}, opts), invalid_options);
      BOOST_CHECK_THROW((void)ps::codec::decode(bytes{0}, opts), invalid_options);
   }
   // A policy-invalid unsigned full message is still rejected while Partial stays a wire value.
   const auto wire = framed({0x12, 3, 0x22, 1, 't', 0x52, 3, 0x0a, 1, 't'});
   const auto received = ps::codec::decode_received(wire, ps::options{});
   BOOST_REQUIRE_EQUAL(received.invalid_messages.size(), 1U);
   BOOST_CHECK(received.value.messages.empty());
   BOOST_REQUIRE(received.value.partial && received.value.partial->subject);
   BOOST_CHECK_EQUAL(received.value.partial->subject->value, "t");
}

BOOST_AUTO_TEST_SUITE_END()
