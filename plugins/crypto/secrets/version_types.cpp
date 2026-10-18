module forge.plugins.crypto.secrets.version_types;

import forge.schema.object;

forge::schema::object_schema<forge::plugins::crypto::secrets::version_policy>
forge::schema::rules<forge::plugins::crypto::secrets::version_policy>::define() {
   using type = forge::plugins::crypto::secrets::version_policy;
   auto schema = forge::schema::object<type>();
   schema.field<&type::purposes>("purposes").max_items(32).each_non_empty();
   schema.field<&type::operations>("operations").max_items(4);
   schema.field<&type::allow_raw_export>("allow-raw-export");
   schema.field<&type::max_plaintext_bytes>("max-plaintext-bytes")
       .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   schema.field<&type::max_ciphertext_bytes>("max-ciphertext-bytes")
       .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   schema.field<&type::max_aad_bytes>("max-aad-bytes")
       .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_store_config>
forge::schema::rules<forge::plugins::crypto::secrets::version_store_config>::define() {
   using type = forge::plugins::crypto::secrets::version_store_config;
   auto schema = forge::schema::object<type>();
   schema.field<&type::enabled>("enabled");
   schema.field<&type::mode>("mode");
   schema.field<&type::path>("path").secret();
   schema.field<&type::bootstrap_passphrase_file>("bootstrap-passphrase-file").secret();
   schema.field<&type::ceiling>("ceiling");
   schema.field<&type::max_secret_ids>("max-secret-ids").range(1, 1024);
   schema.field<&type::max_versions>("max-versions").range(1, 4096);
   schema.field<&type::max_versions_per_secret>("max-versions-per-secret").range(1, 256);
   schema.field<&type::max_material_bytes>("max-material-bytes").range(1, 1048576);
   schema.field<&type::max_total_material_bytes>("max-total-material-bytes").range(1, 33554432);
   schema.field<&type::max_store_plaintext_bytes>("max-store-plaintext-bytes").range(1, 67108864);
   schema.field<&type::max_pending_calls>("max-pending-calls").range(1, 64);
   schema.field<&type::max_pending_bytes>("max-pending-bytes").range(1, 8388608);
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_get_request>
forge::schema::rules<forge::plugins::crypto::secrets::version_get_request>::define() {
   using type = forge::plugins::crypto::secrets::version_get_request;
   auto schema = forge::schema::object<type>();
   schema.field<&type::version>("version");
   schema.field<&type::request>("request").secret();
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_get_result>
forge::schema::rules<forge::plugins::crypto::secrets::version_get_result>::define() {
   using type = forge::plugins::crypto::secrets::version_get_result;
   auto schema = forge::schema::object<type>();
   schema.field<&type::version>("version");
   schema.field<&type::result>("result").secret();
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_derive_request>
forge::schema::rules<forge::plugins::crypto::secrets::version_derive_request>::define() {
   using type = forge::plugins::crypto::secrets::version_derive_request;
   auto schema = forge::schema::object<type>();
   schema.field<&type::version>("version");
   schema.field<&type::request>("request").secret();
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_derive_result>
forge::schema::rules<forge::plugins::crypto::secrets::version_derive_result>::define() {
   using type = forge::plugins::crypto::secrets::version_derive_result;
   auto schema = forge::schema::object<type>();
   schema.field<&type::version>("version");
   schema.field<&type::result>("result").secret();
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_aead_encrypt_request>
forge::schema::rules<forge::plugins::crypto::secrets::version_aead_encrypt_request>::define() {
   using type = forge::plugins::crypto::secrets::version_aead_encrypt_request;
   auto schema = forge::schema::object<type>();
   schema.field<&type::version>("version");
   schema.field<&type::request>("request").secret();
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_aead_encrypt_result>
forge::schema::rules<forge::plugins::crypto::secrets::version_aead_encrypt_result>::define() {
   using type = forge::plugins::crypto::secrets::version_aead_encrypt_result;
   auto schema = forge::schema::object<type>();
   schema.field<&type::version>("version");
   schema.field<&type::result>("result").secret();
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_aead_decrypt_request>
forge::schema::rules<forge::plugins::crypto::secrets::version_aead_decrypt_request>::define() {
   using type = forge::plugins::crypto::secrets::version_aead_decrypt_request;
   auto schema = forge::schema::object<type>();
   schema.field<&type::version>("version");
   schema.field<&type::request>("request").secret();
   return schema;
}

forge::schema::object_schema<forge::plugins::crypto::secrets::version_aead_decrypt_result>
forge::schema::rules<forge::plugins::crypto::secrets::version_aead_decrypt_result>::define() {
   using type = forge::plugins::crypto::secrets::version_aead_decrypt_result;
   auto schema = forge::schema::object<type>();
   schema.field<&type::version>("version");
   schema.field<&type::result>("result").secret();
   return schema;
}
