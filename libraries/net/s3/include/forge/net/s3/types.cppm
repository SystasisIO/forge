module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

export module forge.net.s3.types;

export import forge.crypto.core.secret_string;

export namespace forge::net::s3 {

struct object {
   std::string bucket;
   std::string key;
   std::string version;
};

struct credentials {
   crypto::core::secret_string access_key;
   crypto::core::secret_string secret_key;
   crypto::core::secret_string session_token;
   std::optional<std::chrono::system_clock::time_point> expires;
};

struct config {
   std::string endpoint;
   std::string signing_endpoint;
   std::string region = "us-east-1";
   bool path_style = true;
   credentials identity;
   std::size_t max_calls = 16;
   std::size_t max_connections = 4;
   std::size_t max_memory_bytes = 16 * 1024 * 1024;
   std::uint64_t max_object_bytes = 5ULL * 1024 * 1024 * 1024 * 1024;
   std::uint64_t multipart_threshold = 16 * 1024 * 1024;
   std::size_t part_bytes = 8 * 1024 * 1024;
   std::chrono::milliseconds connect_timeout{5000};
   std::chrono::milliseconds request_timeout{30000};
   std::chrono::milliseconds operation_timeout{300000};
   std::chrono::seconds max_presign_lifetime{3600};
};

struct request_options {
   std::stop_token stop;
   std::optional<std::chrono::steady_clock::time_point> deadline;
};

struct write_options {
   std::string content_type = "application/octet-stream";
   bool if_absent = false;
   std::string if_match;
};

struct byte_range {
   std::uint64_t offset = 0;
   std::uint64_t size = 0;
};

struct read_options {
   std::optional<byte_range> range;
   std::uint64_t max_bytes = 16 * 1024 * 1024;
   std::string if_match;
};

struct metadata {
   std::uint64_t size = 0;
   std::string content_type;
   std::string etag;
   std::string version;
};

enum class method { get, put };

struct signed_url {
   crypto::core::secret_string url;
   std::chrono::system_clock::time_point expires;
};

struct multipart {
   object target;
   std::string id;
};

struct part {
   std::uint32_t number = 0;
   std::uint64_t size = 0;
   std::string etag;
};

struct part_page {
   std::vector<part> parts;
   std::optional<std::uint32_t> next;
};

} // namespace forge::net::s3
