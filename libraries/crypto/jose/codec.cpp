module;

#include <cjose/jws.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>

#if !defined(CJOSE_JWS_EXTERNAL_SIGNING_API) || CJOSE_JWS_EXTERNAL_SIGNING_API != 1
#error "Forge JOSE requires the qualified CJOSE external-signing API"
#endif

module forge.crypto.jose.codec;

import forge.codec.base64;
import forge.codec.json;
import forge.variant.value;

namespace forge::crypto::jose {
namespace {

using jwk_owner = std::unique_ptr<cjose_jwk_t, decltype(&cjose_jwk_release)>;
using header_owner = std::unique_ptr<cjose_header_t, decltype(&cjose_header_release)>;
using jws_owner = std::unique_ptr<cjose_jws_t, decltype(&cjose_jws_release)>;
using prepared_owner = std::unique_ptr<cjose_jws_prepared_t, decltype(&cjose_jws_prepared_release)>;

struct compact_parts {
   std::string header;
   std::string payload;
};

std::vector<std::uint8_t> decode_segment(std::string_view input, std::size_t maximum) {
   if (input.size() > (maximum * 4 + 2) / 3) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "JOSE segment exceeds its byte limit");
   }
   auto decoded = std::vector<std::uint8_t>{};
   try {
      decoded = forge::codec::base64::decode(input, {.characters = forge::codec::base64::alphabet::url,
                                                    .pad = forge::codec::base64::padding_policy::forbid});
   } catch (const forge::codec::base64::exceptions::invalid_input&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_compact, "invalid JOSE Compact encoding");
   }
   if (decoded.size() > maximum) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "JOSE segment exceeds its byte limit");
   }
   return decoded;
}

compact_parts inspect_compact(std::string_view compact, const limits& bounds) {
   if (compact.size() > bounds.compact_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "JOSE Compact exceeds its byte limit");
   }
   const auto first = compact.find('.');
   const auto second = first == compact.npos ? compact.npos : compact.find('.', first + 1);
   if (first == compact.npos || second == compact.npos || first == 0 || second == first + 1 ||
       second + 1 == compact.size() || compact.find('.', second + 1) != compact.npos) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_compact, "JOSE Compact must have three nonempty segments");
   }
   const auto header = decode_segment(compact.substr(0, first), bounds.protected_header_bytes);
   const auto payload = decode_segment(compact.substr(first + 1, second - first - 1), bounds.payload_bytes);
   const auto encoded_signature = compact.substr(second + 1);
   if (encoded_signature.size() != 86) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_signature, "ES256 requires a 64-byte JOSE signature");
   }
   const auto signature = decode_segment(encoded_signature, 64);
   if (signature.size() != 64) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_signature, "ES256 requires a 64-byte JOSE signature");
   }
   return {{header.begin(), header.end()}, {payload.begin(), payload.end()}};
}

std::string check_header(std::string_view header, const header_policy& policy) {
   // The existing JSON codec owns strict syntax, UTF-8 and duplicate detection.
   // Parser diagnostics may contain credential bytes and are deliberately dropped.
   auto options = forge::codec::json::read_options{};
   options.max_depth = 8;
   options.unknown_fields = forge::codec::json::unknown_field_policy::error;
   options.described_records = forge::codec::json::described_record_policy::exact;
   options.max_bytes = policy.bounds.protected_header_bytes;
   options.max_nodes = 512;
   options.max_container_size = 128;
   options.max_string_bytes = 2'048;
   options.reject_nul = true;
   auto parsed = forge::codec::json::read_value(header, options);
   if (!parsed.ok() || !parsed.value.is_object()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_header, "invalid JOSE protected header");
   }
   const auto& object = parsed.value.get_object();
   const auto expected_members = 1U + (policy.typ ? 1U : 0U) + (policy.key_id == key_id_policy::required ? 1U : 0U);
   if (object.size() != expected_members) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_header, "JOSE protected header does not match policy");
   }
   auto kid = std::string{};
   for (const auto& entry : object) {
      if (!entry.value().is_string()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_header, "JOSE header values must be strings");
      }
      const auto value = entry.value().as_string();
      if (entry.key() == "alg") {
         if (value != "ES256") {
            FORGE_THROW_EXCEPTION(exceptions::invalid_header, "JOSE algorithm does not match policy");
         }
      } else if (entry.key() == "typ" && policy.typ) {
         if (value != *policy.typ) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_header, "JOSE type does not match policy");
         }
      } else if (entry.key() == "kid" && policy.key_id == key_id_policy::required) {
         if (value.empty() || value.size() > 128 || !std::ranges::all_of(value, [](unsigned char byte) {
                return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                       (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
             })) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_header, "invalid JOSE key identifier");
         }
         kid = value;
      } else {
         FORGE_THROW_EXCEPTION(exceptions::invalid_header, "JOSE header field is not permitted");
      }
   }
   return kid;
}

jwk_owner public_jwk(const trusted_key& key) {
   auto coordinates = key.public_key;
   auto spec = cjose_jwk_ec_keyspec{};
   spec.crv = CJOSE_JWK_EC_P_256;
   spec.x = reinterpret_cast<std::uint8_t*>(coordinates.data() + 1);
   spec.xlen = 32;
   spec.y = reinterpret_cast<std::uint8_t*>(coordinates.data() + 33);
   spec.ylen = 32;
   // No private component can enter this boundary.
   auto error = cjose_err{};
   auto result = jwk_owner{cjose_jwk_create_EC_spec(&spec, &error), &cjose_jwk_release};
   if (!result) {
      if (error.code == CJOSE_ERR_NO_MEMORY) {
         FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE backend allocation failed");
      }
      FORGE_THROW_EXCEPTION(exceptions::invalid_key, "invalid JOSE verification key");
   }
   return result;
}

} // namespace

struct prepared_message::impl {
   prepared_owner prepared{nullptr, &cjose_jws_prepared_release};
   verified_key_binding binding;
   limits bounds;
   std::string input;
   digest::sha256 hash;
   bool completed = false;
};

prepared_message::prepared_message(std::unique_ptr<impl> state) noexcept : _impl(std::move(state)) {}
prepared_message::prepared_message(prepared_message&&) noexcept = default;
prepared_message& prepared_message::operator=(prepared_message&&) noexcept = default;
prepared_message::~prepared_message() = default;

prepared_message prepare(std::string_view payload, const trusted_key& key, const header_policy& policy) {
   policy.validate();
   if (payload.size() > policy.bounds.payload_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "JOSE payload exceeds its byte limit");
   }
   if (payload.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_compact, "JOSE payload must not be empty");
   }
   if ((policy.key_id == key_id_policy::required) == key.kid.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_key, "JOSE key identifier does not match header policy");
   }
   auto state = std::make_unique<prepared_message::impl>();
   state->binding = identify_key(key);
   state->bounds = policy.bounds;
   auto jwk = public_jwk(key);
   auto error = cjose_err{};
   auto header = header_owner{cjose_header_new(&error), &cjose_header_release};
   if (!header || !cjose_header_set(header.get(), "alg", "ES256", &error) ||
       (policy.typ && !cjose_header_set(header.get(), "typ", policy.typ->c_str(), &error)) ||
       (policy.key_id == key_id_policy::required && !cjose_header_set(header.get(), "kid", key.kid.c_str(), &error))) {
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE header preparation failed");
   }
   state->prepared.reset(cjose_jws_prepare(jwk.get(), header.get(),
                                         reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(), &error));
   if (!state->prepared) {
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE preparation failed");
   }
   const std::uint8_t* input = nullptr;
   const std::uint8_t* digest_bytes = nullptr;
   auto input_size = std::size_t{};
   auto digest_size = std::size_t{};
   if (!cjose_jws_prepared_get_signing_input(state->prepared.get(), &input, &input_size, &error) ||
       !cjose_jws_prepared_get_digest(state->prepared.get(), &digest_bytes, &digest_size, &error) || digest_size != 32) {
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE prepared state is unavailable");
   }
   if (input_size > policy.bounds.compact_bytes || policy.bounds.compact_bytes - input_size < 87) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "JOSE Compact exceeds its byte limit");
   }
   state->input.assign(reinterpret_cast<const char*>(input), input_size);
   const auto encoded_header = std::string_view{state->input}.substr(0, state->input.find('.'));
   const auto decoded_header = decode_segment(encoded_header, policy.bounds.protected_header_bytes);
   static_cast<void>(check_header({reinterpret_cast<const char*>(decoded_header.data()), decoded_header.size()}, policy));
   state->hash = digest::sha256{std::span<const std::uint8_t>{digest_bytes, digest_size}};
   return prepared_message{std::move(state)};
}

std::string_view prepared_message::signing_input() const& {
   if (!_impl) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "JOSE prepared state was moved");
   }
   return _impl->input;
}

digest::sha256 prepared_message::signing_digest() const {
   if (!_impl) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "JOSE prepared state was moved");
   }
   return _impl->hash;
}

const verified_key_binding& prepared_message::key_binding() const& {
   if (!_impl) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "JOSE prepared state was moved");
   }
   return _impl->binding;
}

std::string prepared_message::complete(std::span<const std::uint8_t> signature) {
   if (!_impl || _impl->completed) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "JOSE prepared state is not completable");
   }
   if (signature.size() != 64) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_signature, "ES256 requires a 64-byte JOSE signature");
   }
   auto error = cjose_err{};
   auto jws = jws_owner{cjose_jws_complete(_impl->prepared.get(), signature.data(), signature.size(), &error),
                        &cjose_jws_release};
   if (!jws) {
      if (error.code == CJOSE_ERR_NO_MEMORY) {
         FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE backend allocation failed");
      }
      FORGE_THROW_EXCEPTION(exceptions::invalid_signature, "JOSE signature does not match prepared state");
   }
   _impl->completed = true;
   const char* compact = nullptr;
   if (!cjose_jws_export(jws.get(), &compact, &error) || !compact) {
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE completion export failed");
   }
   const auto size = std::strlen(compact);
   if (size > _impl->bounds.compact_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE completion exceeded prepared bounds");
   }
   return {compact, size};
}

verified_payload::verified_payload(std::string payload, const trusted_keys& snapshot, std::size_t key_index)
    : _payload(std::move(payload)), _snapshot(snapshot), _key_index(key_index) {}

std::string_view verified_payload::payload() const& noexcept { return _payload; }
const verified_key_binding& verified_payload::key_binding() const& { return _snapshot.key_binding(_key_index); }
const trusted_keys& verified_payload::trust_snapshot() const& noexcept { return _snapshot; }

verified_payload verifier::verify(std::string_view compact, const trusted_keys& snapshot) const {
   auto parts = inspect_compact(compact, snapshot.policy().bounds);
   const auto kid = check_header(parts.header, snapshot.policy());
   const auto index = snapshot.find(kid);
   if (!index) {
      FORGE_THROW_EXCEPTION(exceptions::unknown_key, "JOSE key is not in the trust snapshot");
   }
   auto jwk = public_jwk(snapshot.keys()[*index]);
   auto error = cjose_err{};
   auto jws = jws_owner{cjose_jws_import(compact.data(), compact.size(), &error), &cjose_jws_release};
   if (!jws) {
      if (error.code == CJOSE_ERR_NO_MEMORY) {
         FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE backend allocation failed");
      }
      FORGE_THROW_EXCEPTION(exceptions::invalid_compact, "JOSE backend rejected Compact message");
   }
   if (!cjose_jws_verify(jws.get(), jwk.get(), &error)) {
      if (error.code == CJOSE_ERR_NO_MEMORY) {
         FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE backend allocation failed");
      }
      FORGE_THROW_EXCEPTION(exceptions::invalid_signature, "JOSE signature verification failed");
   }
   // Bind the result to the exact authenticated bytes, never a reserialized header/payload.
   std::uint8_t* authenticated = nullptr;
   auto size = std::size_t{};
   if (!cjose_jws_get_plaintext(jws.get(), &authenticated, &size, &error) || size != parts.payload.size() ||
       !authenticated || std::memcmp(authenticated, parts.payload.data(), size) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "JOSE authenticated payload mismatch");
   }
   return verified_payload{std::move(parts.payload), snapshot, *index};
}

} // namespace forge::crypto::jose
