module forge.plugins.crypto.secrets.types;

import forge.schema.object;

forge::schema::object_schema<forge::plugins::crypto::secrets::secret_source>
forge::schema::rules<forge::plugins::crypto::secrets::secret_source>::define() {
   auto schema = forge::schema::object<forge::plugins::crypto::secrets::secret_source>();
   schema.field<&forge::plugins::crypto::secrets::secret_source::type>("type").description("Secret source type");
   schema.field<&forge::plugins::crypto::secrets::secret_source::encoding>("encoding")
       .description("Encoding used by value and file sources");
   schema.field<&forge::plugins::crypto::secrets::secret_source::value>("value").secret().description(
       "Secret value delivered by FORGE config sources");
   schema.field<&forge::plugins::crypto::secrets::secret_source::path>("path").description(
       "Secret file or encrypted_file path");
   schema.field<&forge::plugins::crypto::secrets::secret_source::passphrase_value>("passphrase-value")
       .secret()
       .description("Passphrase for encrypted_file source");
   schema.field<&forge::plugins::crypto::secrets::secret_source::passphrase_file>("passphrase-file")
       .description("Path to passphrase file for encrypted_file source");
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::secret_entry>
forge::schema::rules<forge::plugins::crypto::secrets::secret_entry>::define() {
   auto schema = forge::schema::object<forge::plugins::crypto::secrets::secret_entry>();
   schema.field<&forge::plugins::crypto::secrets::secret_entry::id>("id").required().non_empty();
   schema.field<&forge::plugins::crypto::secrets::secret_entry::kind>("kind").description("Secret material kind");
   schema.field<&forge::plugins::crypto::secrets::secret_entry::source>("source").required().secret();
   schema.field<&forge::plugins::crypto::secrets::secret_entry::purposes>("purposes").min_items(1).each_non_empty();
   schema.field<&forge::plugins::crypto::secrets::secret_entry::operations>("operations").min_items(1);
   schema.field<&forge::plugins::crypto::secrets::secret_entry::allow_raw_export>("allow-raw-export")
       .default_value(false);
   schema.field<&forge::plugins::crypto::secrets::secret_entry::max_plaintext_bytes>("max-plaintext-bytes")
       .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   schema.field<&forge::plugins::crypto::secrets::secret_entry::max_ciphertext_bytes>("max-ciphertext-bytes")
       .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   schema.field<&forge::plugins::crypto::secrets::secret_entry::max_aad_bytes>("max-aad-bytes")
       .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::config>
forge::schema::rules<forge::plugins::crypto::secrets::config>::define() {
   auto schema = forge::schema::object<forge::plugins::crypto::secrets::config>();
   schema.field<&forge::plugins::crypto::secrets::config::secrets>("secrets")
       .items<forge::plugins::crypto::secrets::secret_entry>()
       .secret()
       .unique_by<&forge::plugins::crypto::secrets::secret_entry::id>();
   schema.field<&forge::plugins::crypto::secrets::config::default_max_plaintext_bytes>("default-max-plaintext-bytes")
       .default_value(forge::plugins::crypto::secrets::default_max_plaintext_bytes)
       .range(1, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   schema
       .field<&forge::plugins::crypto::secrets::config::default_max_ciphertext_bytes>("default-max-ciphertext-bytes")
       .default_value(forge::plugins::crypto::secrets::default_max_ciphertext_bytes)
       .range(1, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   schema.field<&forge::plugins::crypto::secrets::config::default_max_aad_bytes>("default-max-aad-bytes")
       .default_value(forge::plugins::crypto::secrets::default_max_aad_bytes)
       .range(1, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   schema.field<&forge::plugins::crypto::secrets::config::encrypted_file_max_scrypt_n>("encrypted-file-max-scrypt-n")
       .default_value(forge::plugins::crypto::secrets::default_encrypted_file_max_scrypt_n)
       .range(1, 1ULL << 32);
   schema.field<&forge::plugins::crypto::secrets::config::encrypted_file_max_scrypt_r>("encrypted-file-max-scrypt-r")
       .default_value(forge::plugins::crypto::secrets::default_encrypted_file_max_scrypt_r)
       .range(1, 1ULL << 32);
   schema.field<&forge::plugins::crypto::secrets::config::encrypted_file_max_scrypt_p>("encrypted-file-max-scrypt-p")
       .default_value(forge::plugins::crypto::secrets::default_encrypted_file_max_scrypt_p)
       .range(1, 1ULL << 32);
   schema
       .field<&forge::plugins::crypto::secrets::config::encrypted_file_max_scrypt_memory_bytes>(
           "encrypted-file-max-scrypt-memory-bytes")
       .default_value(forge::plugins::crypto::secrets::default_encrypted_file_max_scrypt_memory_bytes)
       .range(1, 1ULL << 32);
   return schema;
}
