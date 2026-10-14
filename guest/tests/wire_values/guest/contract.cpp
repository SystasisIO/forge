#include <cstdint>
#include <vector>

import forge.contract;
import forge.raw.codec;
import fixture.wire_values;

class [[forge::contract("wirevalues")]] wirevalues : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] forge::api::core::status checkstatus(forge::api::core::status value) { return value; }
   [[forge::action]] forge::api::core::frame_kind checkframe(forge::api::core::frame_kind value) { return value; }
   [[forge::action]] forge::api::core::method_kind checkkind(forge::api::core::method_kind value) { return value; }
   [[forge::action]] forge::api::core::stream_direction checkdir(forge::api::core::stream_direction value) { return value; }
   [[forge::action]] forge::api::core::capability checkcap(forge::api::core::capability value) { return value; }
   [[forge::action]] forge::api::core::surface checksurface(forge::api::core::surface value) { return value; }
   [[forge::action]] forge::net::http::method checkmethod(forge::net::http::method value) {
      forge::contract::check(forge::net::http::is_idempotent(forge::net::http::method::get), "method linkage");
      return value;
   }
   [[forge::action]] fixture::packet echo(fixture::packet value) {
      return forge::raw::unpack_exact<fixture::packet>(forge::raw::pack(value));
   }
   [[forge::action]] fixture::packet decode(std::vector<std::uint8_t> value) {
      return forge::raw::unpack_exact<fixture::packet>(value);
   }
   [[forge::action]] fixture::legacy_value legacy(fixture::legacy_value value) { return value; }
   [[forge::action]] fixture::signed_value signednum(fixture::signed_value value) { return value; }
   [[forge::action]] fixture::unsigned_value unsignednum(fixture::unsigned_value value) { return value; }
   [[forge::action]] fixture::narrow_value narrownum(fixture::narrow_value value) { return value; }
   [[forge::action]] fixture::signed_extreme wideint(fixture::signed_extreme value) { return value; }
   [[forge::action]] fixture::narrow_value badnarrow() { return static_cast<fixture::narrow_value>(128); }
   [[forge::action]] fixture::packet fresh() {
      return {
         .error = {
            .error = "E", .message = "msg", .retryable = true,
            .status_code = forge::api::core::status::conflict,
            .identity = {.category = "cat", .code = 0x01020304U},
            .details_codec = forge::api::core::codec_id{.value = "raw"},
            .details = forge::api::core::bytes{0x00U, 0x80U, 0xffU},
         },
         .verb = forge::net::http::method::post,
         .statuses = {forge::api::core::status::invalid_argument, forge::api::core::status::bad_gateway},
         .fixed = {forge::api::core::status::invalid_argument, forge::api::core::status::bad_gateway},
         .optional = forge::api::core::status::ok,
         .choice = forge::net::http::method::get,
      };
   }
};
