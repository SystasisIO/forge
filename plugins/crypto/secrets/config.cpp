module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.crypto.secrets.plugin;

import forge.config.core.component;
import forge.config.core.decode;
import forge.codec.base64;
import forge.codec.hex;
import forge.crypto.keystore.encrypted_file;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.secret_string;
import forge.crypto.core.types;
import forge.exceptions;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"
#include "details/require_complete_file_read.hxx"

namespace forge::plugins::crypto::secrets {
namespace {

[[nodiscard]] std::string trim_ascii(std::string value) {
   auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
   value.erase(value.begin(), std::ranges::find_if(value, not_space));
   value.erase(std::ranges::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
   return value;
}

[[nodiscard]] forge::crypto::core::bytes bytes_from_string(std::string_view value) {
   return forge::crypto::core::bytes{value.begin(), value.end()};
}

[[nodiscard]] forge::crypto::core::bytes decode_material(std::string value, encoding encoding_value,
                                                         const std::string& id) {
   try {
      switch (encoding_value) {
      case encoding::raw:
         return bytes_from_string(value);
      case encoding::hex: {
         value = trim_ascii(std::move(value));
         if ((value.size() % 2U) != 0U) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_secret, "hex secret has odd length",
                                  forge::exceptions::ctx("secret_id", id));
         }
         auto output = forge::crypto::core::bytes(value.size() / 2U);
         const auto written = forge::codec::hex::decode(value, output);
         output.resize(written);
         return output;
      }
      case encoding::base64: {
         value = trim_ascii(std::move(value));
         return forge::codec::base64::decode(value);
      }
      }
   } catch (const exceptions::invalid_secret&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_secret, "secret material cannot be decoded",
                            forge::exceptions::ctx("secret_id", id));
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_secret, "unknown secret encoding",
                         forge::exceptions::ctx("secret_id", id));
}

[[nodiscard]] forge::crypto::core::bytes read_file(const std::string& path, std::uint64_t max_bytes,
                                                   const std::string& id) {
   if (path.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_source, "secret file path is empty",
                            forge::exceptions::ctx("secret_id", id));
   }
   auto input = std::ifstream{std::filesystem::path{path}, std::ios::binary};
   if (!input) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_source, "secret file cannot be opened",
                            forge::exceptions::ctx("secret_id", id), forge::exceptions::ctx("path", path));
   }
   input.seekg(0, std::ios::end);
   const auto size = input.tellg();
   if (size < 0 || static_cast<std::uint64_t>(size) > max_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "secret file exceeds configured limit",
                            forge::exceptions::ctx("secret_id", id));
   }
   input.seekg(0, std::ios::beg);
   auto output = forge::crypto::core::bytes(static_cast<std::size_t>(size));
   if (!output.empty()) {
      input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
      require_complete_file_read(input, output.size(), path, id);
   }
   return output;
}

[[nodiscard]] forge::crypto::core::secret_string read_passphrase(const secret_source& source, const std::string& id) {
   if (!source.passphrase_value.empty()) {
      return forge::crypto::core::secret_string{source.passphrase_value};
   }
   if (!source.passphrase_file.empty()) {
      const auto value = read_file(source.passphrase_file, 64U * 1024U, id);
      return forge::crypto::core::secret_string{trim_ascii(std::string{value.begin(), value.end()})};
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_source, "encrypted_file source requires passphrase",
                         forge::exceptions::ctx("secret_id", id));
}

[[nodiscard]] forge::crypto::core::secret_bytes
load_secret_material(const secret_entry& entry, std::uint64_t max_plaintext_bytes, std::uint64_t max_ciphertext_bytes,
                     forge::crypto::keystore::decrypt_limits decrypt_limits) {
   auto material = forge::crypto::core::bytes{};
   switch (entry.source.type) {
   case source_type::value:
      material = decode_material(entry.source.value, entry.source.encoding, entry.id);
      break;
   case source_type::file: {
      auto value = read_file(entry.source.path, max_ciphertext_bytes, entry.id);
      material = decode_material(std::string{value.begin(), value.end()}, entry.source.encoding, entry.id);
      break;
   }
   case source_type::encrypted_file: {
      auto container = read_file(entry.source.path, max_ciphertext_bytes, entry.id);
      auto passphrase = read_passphrase(entry.source, entry.id);
      try {
         decrypt_limits.max_plaintext_bytes = max_plaintext_bytes;
         material = forge::crypto::keystore::decrypt_file(container, passphrase, decrypt_limits).copy();
      } catch (const forge::crypto::keystore::exceptions::size_limit_exceeded&) {
         FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded,
                               "encrypted secret file source exceeds configured plaintext limit",
                               forge::exceptions::ctx("secret_id", entry.id));
      } catch (const forge::crypto::keystore::exceptions::invalid_file&) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_secret, "encrypted secret file cannot be decrypted",
                               forge::exceptions::ctx("secret_id", entry.id));
      }
      break;
   }
   }
   if (material.size() > max_plaintext_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "secret material exceeds configured plaintext limit",
                            forge::exceptions::ctx("secret_id", entry.id));
   }
   if (material.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_secret, "secret material is empty",
                            forge::exceptions::ctx("secret_id", entry.id));
   }
   return forge::crypto::core::secret_bytes{std::move(material)};
}

[[nodiscard]] std::uint64_t resolved_limit(std::uint64_t value, std::uint64_t fallback) {
   return value == 0 ? fallback : value;
}

void require_aes_update_limit(std::uint64_t value, const char* label) {
   if (value > aes_update_bytes_ceiling) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, std::string{label} + " exceeds AES update ceiling");
   }
}

} // namespace

config decode_config(const forge::config::core::component_view& view) {
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, forge::config::core::format_decode_diagnostics(
                                                            "invalid crypto secrets config", decoded.diagnostics));
   }
   return std::move(decoded.value);
}

void apply_config(plugin::impl& state, forge::config::core::component_view view) {
   const auto generation = state.lifecycle.snapshot().generation;
   auto decoded = decode_config(view);
   require_aes_update_limit(decoded.default_max_plaintext_bytes, "default-max-plaintext-bytes");
   require_aes_update_limit(decoded.default_max_ciphertext_bytes, "default-max-ciphertext-bytes");
   require_aes_update_limit(decoded.default_max_aad_bytes, "default-max-aad-bytes");

   auto loaded = std::map<std::string, loaded_secret>{};
   const auto decrypt_limits = forge::crypto::keystore::decrypt_limits{
       .max_plaintext_bytes = decoded.default_max_plaintext_bytes,
       .max_scrypt_n = decoded.encrypted_file_max_scrypt_n,
       .max_scrypt_r = decoded.encrypted_file_max_scrypt_r,
       .max_scrypt_p = decoded.encrypted_file_max_scrypt_p,
       .max_scrypt_memory_bytes = decoded.encrypted_file_max_scrypt_memory_bytes,
   };
   for (auto& entry : decoded.secrets) {
      auto max_plaintext = resolved_limit(entry.max_plaintext_bytes, decoded.default_max_plaintext_bytes);
      auto max_ciphertext = resolved_limit(entry.max_ciphertext_bytes, decoded.default_max_ciphertext_bytes);
      auto max_aad = resolved_limit(entry.max_aad_bytes, decoded.default_max_aad_bytes);
      require_aes_update_limit(max_plaintext, "max-plaintext-bytes");
      require_aes_update_limit(max_ciphertext, "max-ciphertext-bytes");
      require_aes_update_limit(max_aad, "max-aad-bytes");
      auto material = load_secret_material(entry, max_plaintext, max_ciphertext, decrypt_limits);
      loaded.emplace(entry.id, loaded_secret{
                                   .id = entry.id,
                                   .kind = entry.kind,
                                   .material = std::move(material),
                                   .purposes = std::move(entry.purposes),
                                   .operations = std::move(entry.operations),
                                   .allow_raw_export = entry.allow_raw_export,
                                   .max_plaintext_bytes = max_plaintext,
                                   .max_ciphertext_bytes = max_ciphertext,
                                   .max_aad_bytes = max_aad,
                               });
   }
   auto candidate = std::shared_ptr<const std::map<std::string, loaded_secret>>{
       std::make_shared<std::map<std::string, loaded_secret>>(std::move(loaded))};
   auto retired = std::shared_ptr<const std::map<std::string, loaded_secret>>{};
   if (!state.lifecycle.try_admit(generation, [&] {
      retired = std::atomic_exchange_explicit(&state.secrets, candidate, std::memory_order_acq_rel);
   })) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "Secrets configuration was stopped");
   }
}

} // namespace forge::plugins::crypto::secrets
