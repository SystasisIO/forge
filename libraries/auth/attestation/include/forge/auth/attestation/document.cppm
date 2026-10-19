module;

#include <boost/describe.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

export module forge.auth.attestation.document;

export import forge.auth.attestation.verification_policy;

export namespace forge::auth::attestation {

namespace detail {

void validate_document_fields(std::string_view iss, std::string_view aud, std::string_view jti,
                              std::int64_t iat, std::int64_t nbf, std::int64_t exp, std::int64_t version,
                              std::string_view purpose, std::string_view schema,
                              const verification_policy& expected, std::chrono::sys_seconds now);

} // namespace detail

template <typename Claims> struct extension {
   std::int64_t v = profile_version;
   std::string purpose;
   std::string schema;
   Claims claims{};

   bool operator==(const extension&) const = default;

   BOOST_DESCRIBE_CLASS(extension, (), (v, purpose, schema, claims), (), ())
};

template <typename Claims> struct document {
   std::string iss;
   std::string aud;
   std::string jti;
   std::int64_t iat = 0;
   std::int64_t nbf = 0;
   std::int64_t exp = 0;
   extension<Claims> att;

   void validate(const verification_policy& expected, std::chrono::sys_seconds now) const {
      detail::validate_document_fields(iss, aud, jti, iat, nbf, exp, att.v, att.purpose, att.schema, expected, now);
   }

   bool operator==(const document&) const = default;

   BOOST_DESCRIBE_CLASS(document, (), (iss, aud, jti, iat, nbf, exp, att), (), ())
};

} // namespace forge::auth::attestation
