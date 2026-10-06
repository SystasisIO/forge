#include "details/quic_engine_support.hxx"

namespace forge::net::quic::detail {
engine_failure::engine_failure(engine_error_kind kind, std::string message)
    : kind_(kind), message_(std::move(message)) {}

engine_error_kind engine_failure::kind() const noexcept {
   return kind_;
}

const char* engine_failure::what() const noexcept {
   return message_.c_str();
}

const std::string& engine_failure::message() const noexcept {
   return message_;
}

std::string normalize_engine_sha256_fingerprint(std::string_view value) {
   auto normalized = std::string{};
   normalized.reserve(value.size());
   for (const auto ch : value) {
      if (ch == ':' || ch == '-' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
         continue;
      }
      if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
         throw_engine(engine_error_kind::invalid_options, "invalid SHA-256 fingerprint");
      }
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
   }
   if (normalized.size() != 64) {
      throw_engine(engine_error_kind::invalid_options, "SHA-256 fingerprint must contain 32 bytes");
   }
   return normalized;
}

std::string engine_sha256_fingerprint(std::span<const std::uint8_t> data) {
   const auto fingerprint = forge::crypto::digest::sha256::hash(data);
   return forge::codec::hex::encode(fingerprint.to_uint8_span());
}

} // namespace forge::net::quic::detail
