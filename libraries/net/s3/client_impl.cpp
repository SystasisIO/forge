module;

#include <forge/exceptions/macros.hpp>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/crt/io/Bootstrap.h>
#include <aws/crt/io/EventLoopGroup.h>
#include <aws/crt/io/HostResolver.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListPartsRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/UploadPartRequest.h>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/post.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <streambuf>
#include <sstream>
#include <variant>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

module forge.net.s3.client;

import forge.asio.compute;

#include "details/backend_call.hxx"
#include "details/client_impl.hxx"

namespace forge::net::s3 {

namespace {

Aws::String aws_string(std::string_view value) {
   return {value.data(), value.size()};
}
std::string string(const Aws::String& value) {
   return {value.data(), value.size()};
}

void validate_object(const object& target, bool version_allowed = true) {
   const auto alphanumeric = [](char value) {
      return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
   };
   if (target.bucket.size() < 3 || target.bucket.size() > 63 || target.key.empty() || target.key.size() > 1024 ||
       target.version.size() > 1024 || (!version_allowed && !target.version.empty()) ||
       !alphanumeric(target.bucket.front()) || !alphanumeric(target.bucket.back()) ||
       !std::all_of(target.bucket.begin(), target.bucket.end(),
                    [&](char value) { return alphanumeric(value) || value == '-' || value == '.'; }) ||
       target.bucket.find("..") != std::string::npos || target.bucket.find(".-") != std::string::npos ||
       target.bucket.find("-.") != std::string::npos || target.key.find('\0') != std::string::npos ||
       target.version.find('\0') != std::string::npos) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 object identity is invalid");
   }
}

void validate_write(const write_options& write) {
   if (write.content_type.empty() || write.content_type.size() > 256 ||
       write.content_type.find_first_of("\r\n") != std::string::npos || write.if_match.size() > 1024 ||
       write.if_match.find_first_of("\r\n") != std::string::npos || (write.if_absent && !write.if_match.empty())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 write options are invalid");
   }
}

void validate_session(const multipart& session) {
   validate_object(session.target, false);
   if (session.id.empty() || session.id.size() > 2048 || session.id.find('\0') != std::string::npos) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 multipart identity is invalid");
   }
}

template <typename Request> void set_object(Request& request, const object& target) {
   request.SetBucket(aws_string(target.bucket));
   request.SetKey(aws_string(target.key));
   if constexpr (requires { request.SetVersionId(Aws::String{}); }) {
      if (!target.version.empty()) {
         request.SetVersionId(aws_string(target.version));
      }
   }
}

template <typename Request> void set_conditions(Request& request, const write_options& write) {
   if (write.if_absent) {
      request.SetIfNoneMatch("*");
   }
   if (!write.if_match.empty()) {
      request.SetIfMatch(aws_string(write.if_match));
   }
}

template <typename Result> metadata describe(const Result& result, std::uint64_t size) {
   metadata value;
   value.size = size;
   value.etag = string(result.GetETag());
   value.version = string(result.GetVersionId());
   if constexpr (requires { result.GetContentType(); }) {
      value.content_type = string(result.GetContentType());
   }
   return value;
}

void validate_range(const read_options& read, std::uint64_t maximum) {
   if (read.max_bytes == 0 || read.max_bytes > maximum || read.if_match.size() > 1024 ||
       read.if_match.find_first_of("\r\n") != std::string::npos) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 read limit or condition is invalid");
   }
   if (read.range && (read.range->size == 0 || read.range->size > read.max_bytes ||
                      read.range->offset > std::numeric_limits<std::uint64_t>::max() - read.range->size)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 byte range is invalid");
   }
}

void set_read(Aws::S3::Model::GetObjectRequest& request, const object& target, const read_options& read) {
   set_object(request, target);
   if (!read.if_match.empty()) {
      request.SetIfMatch(aws_string(read.if_match));
   }
   if (read.range) {
      request.SetRange(aws_string("bytes=" + std::to_string(read.range->offset) + "-" +
                                  std::to_string(read.range->offset + read.range->size - 1)));
   }
}

void check_read(const Aws::S3::Model::GetObjectResult& result, const read_options& read, std::uint64_t size) {
   if (result.GetContentLength() < 0 || static_cast<std::uint64_t>(result.GetContentLength()) != size ||
       (read.range && (size != read.range->size ||
                       !string(result.GetContentRange())
                            .starts_with("bytes " + std::to_string(read.range->offset) + "-" +
                                         std::to_string(read.range->offset + read.range->size - 1) + "/")))) {
      FORGE_THROW_EXCEPTION(exceptions::transport,
                            "S3 response length or Content-Range does not match the requested content");
   }
}

} // namespace

struct client::impl::backend {
   struct sdk_scope {
      struct state {
         std::mutex mutex;
         std::size_t owners = 0;
         Aws::SDKOptions options;
      };

      static state& shared() {
         static state value;
         return value;
      }

      sdk_scope() {
         auto& value = shared();
         const auto lock = std::scoped_lock{value.mutex};
         if (value.owners == 0) {
            value.options.httpOptions.compliantRfc3986Encoding = true;
            value.options.httpOptions.preservePathSeparators = true;
            value.options.ioOptions.clientBootstrap_create_fn = [] {
               auto loops = std::make_shared<Aws::Crt::Io::EventLoopGroup>(1);
               auto resolver = std::make_shared<Aws::Crt::Io::DefaultHostResolver>(*loops, 8, 30);
               auto* bootstrap = Aws::New<Aws::Crt::Io::ClientBootstrap>("forge.s3", *loops, *resolver);
               bootstrap->EnableBlockingShutdown();
               return std::shared_ptr<Aws::Crt::Io::ClientBootstrap>{
                   bootstrap,
                   [loops = std::move(loops), resolver = std::move(resolver)](auto* value) { Aws::Delete(value); }};
            };
            Aws::InitAPI(value.options);
         }
         ++value.owners;
      }

      ~sdk_scope() {
         auto& value = shared();
         const auto lock = std::scoped_lock{value.mutex};
         if (--value.owners == 0) {
            Aws::ShutdownAPI(value.options);
         }
      }
   };

   struct unused_executor final : Aws::Utils::Threading::Executor {
      bool SubmitToThread(std::function<void()>&&) override {
         return false;
      }
   };

   struct provider final : Aws::Auth::AWSCredentialsProvider {
      std::mutex mutex;
      credentials identity;

      explicit provider(credentials value) {
         update(std::move(value));
      }

      void update(credentials value) {
         if (value.access_key.empty() || value.secret_key.empty() || value.access_key.size() > 256 ||
             value.secret_key.size() > 4096 || value.session_token.size() > 16384 ||
             (value.expires && *value.expires <= std::chrono::system_clock::now())) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 credentials are absent, expired, or exceed limits");
         }
         const auto lock = std::scoped_lock{mutex};
         identity = std::move(value);
      }

      Aws::Auth::AWSCredentials GetAWSCredentials() override {
         const auto lock = std::scoped_lock{mutex};
         // AWS identity resolution has noexcept boundaries. Preflight expiry
         // outside SDK; always return nonempty credentials so no unsigned fallback occurs.
         return {aws_string(identity.access_key.view()), aws_string(identity.secret_key.view()),
                 aws_string(identity.session_token.view())};
      }

      void require_valid() {
         const auto lock = std::scoped_lock{mutex};
         if (identity.expires && *identity.expires <= std::chrono::system_clock::now()) {
            FORGE_THROW_EXCEPTION(exceptions::denied, "S3 credentials expired before request signing");
         }
      }

      std::chrono::system_clock::time_point expiry(std::chrono::system_clock::time_point desired) {
         const auto lock = std::scoped_lock{mutex};
         return identity.expires ? std::min(desired, *identity.expires) : desired;
      }
   };

   struct source final : std::streambuf {
      std::vector<std::byte> bytes;
      std::ifstream file;
      std::array<char, 64 * 1024> buffer;
      std::uint64_t base = 0;
      std::uint64_t size = 0;
      std::uint64_t cursor = 0;
      activity& call;

      source(std::variant<std::vector<std::byte>, std::filesystem::path> input, std::optional<byte_range> range,
             std::uint64_t maximum, std::size_t memory_limit, activity& activity)
          : call{activity} {
         if (auto* value = std::get_if<std::vector<std::byte>>(&input)) {
            bytes = std::move(*value);
            size = bytes.size();
            if (size > memory_limit || range) {
               FORGE_THROW_EXCEPTION(exceptions::limit, "S3 memory upload exceeds its limit");
            }
         } else {
            const auto& path = std::get<std::filesystem::path>(input);
            file.open(path, std::ios::binary | std::ios::ate);
            std::error_code error;
            const auto regular = std::filesystem::is_regular_file(path, error);
            if (!file || file.tellg() < 0 || !regular || error) {
               FORGE_THROW_EXCEPTION(exceptions::io, "S3 upload source is not a readable regular file",
                                     forge::exceptions::ctx("filesystem_status", error.value()));
            }
            size = static_cast<std::uint64_t>(file.tellg());
            if (range) {
               if (range->size == 0 || range->offset > size || range->size > size - range->offset) {
                  FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 source range exceeds the file");
               }
               base = range->offset;
               size = range->size;
            }
            file.seekg(static_cast<std::streamoff>(base));
         }
         if (size > maximum || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            FORGE_THROW_EXCEPTION(exceptions::limit, "S3 upload source exceeds its limit");
         }
         setg(buffer.data(), buffer.data(), buffer.data());
      }

      std::uint64_t position() const {
         return cursor - static_cast<std::uint64_t>(egptr() - gptr());
      }

      int_type underflow() override {
         if (call.interrupted()) {
            return traits_type::eof();
         }
         if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
         }
         if (cursor == size) {
            return traits_type::eof();
         }
         const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - cursor));
         if (file.is_open()) {
            file.read(buffer.data(), static_cast<std::streamsize>(count));
            if (static_cast<std::size_t>(file.gcount()) != count) {
               return traits_type::eof();
            }
         } else {
            std::memcpy(buffer.data(), bytes.data() + cursor, count);
         }
         cursor += count;
         setg(buffer.data(), buffer.data(), buffer.data() + count);
         return traits_type::to_int_type(*gptr());
      }

      pos_type seekoff(off_type offset, std::ios::seekdir origin, std::ios::openmode mode) override {
         if (!(mode & std::ios::in)) {
            return pos_type{off_type{-1}};
         }
         const auto start = origin == std::ios::beg ? 0 : origin == std::ios::end ? size : position();
         if ((offset < 0 && static_cast<std::uint64_t>(-(offset + 1)) + 1 > start) ||
             (offset >= 0 && static_cast<std::uint64_t>(offset) > size - start)) {
            return pos_type{off_type{-1}};
         }
         cursor = static_cast<std::uint64_t>(static_cast<off_type>(start) + offset);
         setg(buffer.data(), buffer.data(), buffer.data());
         if (file.is_open()) {
            file.clear();
            file.seekg(static_cast<std::streamoff>(base + cursor));
            if (!file) {
               return pos_type{off_type{-1}};
            }
         }
         return pos_type{static_cast<off_type>(cursor)};
      }

      pos_type seekpos(pos_type position, std::ios::openmode mode) override {
         return seekoff(static_cast<off_type>(position), std::ios::beg, mode);
      }
   };

   struct sink final : std::streambuf {
      std::vector<std::byte> bytes;
      std::filesystem::path temporary;
      std::FILE* file = nullptr;
      std::uint64_t maximum;
      std::uint64_t size = 0;
      bool exceeded = false;
      bool failed = false;

      explicit sink(std::uint64_t maximum) : maximum{maximum} {}

      sink(std::uint64_t maximum, const std::filesystem::path& destination) : maximum{maximum} {
         static std::atomic<std::uint64_t> sequence{0};
         for (unsigned retry = 0; retry != 8 && file == nullptr; ++retry) {
            temporary = destination;
            temporary += ".s3-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                         std::to_string(++sequence);
            file = std::fopen(temporary.string().c_str(), "wbx");
         }
         if (file == nullptr) {
            FORGE_THROW_EXCEPTION(exceptions::io, "S3 download temporary file cannot be created");
         }
      }

      ~sink() {
         if (file) {
            std::fclose(file);
         }
         if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
         }
      }

      std::streamsize xsputn(const char* data, std::streamsize count) override {
         if (count < 0 || static_cast<std::uint64_t>(count) > maximum - size) {
            exceeded = true;
            return 0;
         }
         if (file) {
            if (std::fwrite(data, 1, static_cast<std::size_t>(count), file) != static_cast<std::size_t>(count)) {
               failed = true;
               return 0;
            }
         } else {
            const auto* begin = reinterpret_cast<const std::byte*>(data);
            bytes.insert(bytes.end(), begin, begin + count);
         }
         size += static_cast<std::uint64_t>(count);
         return count;
      }

      int_type overflow(int_type value) override {
         if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
         }
         const auto byte = traits_type::to_char_type(value);
         return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
      }

      void publish(const std::filesystem::path& destination) {
         if (!file || std::fflush(file) != 0) {
            FORGE_THROW_EXCEPTION(exceptions::io, "S3 download flush failed");
         }
#ifdef _WIN32
         const auto synced = _commit(_fileno(file));
#else
         const auto synced = ::fsync(::fileno(file));
#endif
         const auto closed = std::fclose(file);
         file = nullptr;
         if (synced != 0 || closed != 0) {
            FORGE_THROW_EXCEPTION(exceptions::io, "S3 download sync failed");
         }
         std::error_code error;
         std::filesystem::rename(temporary, destination, error);
         if (error) {
            FORGE_THROW_EXCEPTION(exceptions::io, "S3 download destination cannot be published");
         }
         temporary.clear();
      }
   };

   struct response final : std::streambuf {
      std::stringbuf buffer;
      std::atomic<bool>& exceeded;
      std::size_t written = 0;
      static constexpr std::size_t maximum = 1024 * 1024;

      explicit response(std::atomic<bool>& flag) : exceeded{flag} {}

      std::streamsize xsputn(const char* bytes, std::streamsize size) override {
         if (size < 0 || static_cast<std::size_t>(size) > maximum - written) {
            exceeded.store(true);
            return 0;
         }
         const auto count = buffer.sputn(bytes, size);
         written += static_cast<std::size_t>(count);
         return count;
      }

      int_type overflow(int_type value) override {
         if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
         }
         const auto byte = traits_type::to_char_type(value);
         return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
      }

      int_type underflow() override {
         return buffer.sgetc();
      }
      int_type uflow() override {
         return buffer.sbumpc();
      }
      std::streamsize xsgetn(char* bytes, std::streamsize size) override {
         return buffer.sgetn(bytes, size);
      }
      std::streamsize showmanyc() override {
         return buffer.in_avail();
      }
      pos_type seekoff(off_type value, std::ios::seekdir origin, std::ios::openmode mode) override {
         return buffer.pubseekoff(value, origin, mode);
      }
      pos_type seekpos(pos_type value, std::ios::openmode mode) override {
         return buffer.pubseekpos(value, mode);
      }
   };

   sdk_scope sdk;
   std::shared_ptr<provider> identity;
   std::shared_ptr<Aws::S3::S3Client> client;
   std::shared_ptr<Aws::S3::S3Client> signing;
   std::mutex signing_mutex;

   explicit backend(const config& options) {
      identity = Aws::MakeShared<provider>("forge.s3", options.identity);
      Aws::S3::S3ClientConfiguration native;
      native.region = aws_string(options.region);
      native.endpointOverride = aws_string(options.endpoint);
      native.useVirtualAddressing = !options.path_style;
      native.disableS3ExpressAuth = true;
      native.maxConnections = static_cast<unsigned>(options.max_connections);
      native.connectTimeoutMs = static_cast<long>(options.connect_timeout.count());
      native.requestTimeoutMs = static_cast<long>(options.request_timeout.count());
      native.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>("forge.s3", 0);
      native.executor = Aws::MakeShared<unused_executor>("forge.s3");
      native.verifySSL = true;
      native.followRedirects = Aws::Client::FollowRedirectsPolicy::NEVER;
      native.checksumConfig.requestChecksumCalculation = Aws::Client::RequestChecksumCalculation::WHEN_REQUIRED;
      native.checksumConfig.responseChecksumValidation = Aws::Client::ResponseChecksumValidation::WHEN_SUPPORTED;
      client = Aws::MakeShared<Aws::S3::S3Client>("forge.s3", identity, nullptr, native);
      if (!options.signing_endpoint.empty() && options.signing_endpoint != options.endpoint) {
         native.endpointOverride = aws_string(options.signing_endpoint);
         signing = Aws::MakeShared<Aws::S3::S3Client>("forge.s3", identity, nullptr, native);
      } else {
         signing = client;
      }
   }

   template <typename Request> static void bind(Request& request, activity& call) {
      call.check();
      call.owner->_backend->identity->require_valid();
      if constexpr (!std::is_same_v<Request, Aws::S3::Model::GetObjectRequest>) {
         auto output = std::make_shared<response>(call.response_exceeded);
         request.SetResponseStreamFactory([output] { return Aws::New<Aws::IOStream>("forge.s3", output.get()); });
      }
      request.SetContinueRequestHandler([&call](const Aws::Http::HttpRequest*) { return !call.interrupted(); });
      request.SetRequestSignedHandler([&call](const Aws::Http::HttpRequest&) { call.started.store(true); });
   }

   template <typename Outcome> static void check(const Outcome& outcome, activity& call) {
      if (call.response_exceeded.load()) {
         if (call.mutating && call.started.load()) {
            FORGE_THROW_EXCEPTION(exceptions::unknown_outcome,
                                  "S3 mutation response exceeded the control response limit");
         }
         FORGE_THROW_EXCEPTION(exceptions::limit, "S3 control response exceeded 1 MiB");
      }
      if (outcome.IsSuccess()) {
         return;
      }
      const auto& error = outcome.GetError();
      const auto status = static_cast<unsigned>(error.GetResponseCode());
      if (status == 404) {
         FORGE_THROW_EXCEPTION(exceptions::not_found, "S3 object or upload does not exist");
      }
      if (status == 401 || status == 403) {
         FORGE_THROW_EXCEPTION(exceptions::denied, "S3 request was denied");
      }
      if (status == 409 || status == 412) {
         FORGE_THROW_EXCEPTION(exceptions::conflict, "S3 write condition failed");
      }
      if (status >= 400 && status < 500 && status != 408 && status != 429) {
         FORGE_THROW_EXCEPTION(exceptions::service, "S3 rejected the request",
                               forge::exceptions::ctx("status", status));
      }
      if (call.mutating && call.started.load()) {
         FORGE_THROW_EXCEPTION(exceptions::unknown_outcome,
                               "S3 mutation has no confirmed result; reconcile before retry");
      }
      call.check();
      if (status < 400) {
         FORGE_THROW_EXCEPTION(exceptions::transport, "S3 transport or response integrity failed");
      }
      FORGE_THROW_EXCEPTION(exceptions::service, "S3 service failed", forge::exceptions::ctx("status", status));
   }

   void check_sink(const sink& output) {
      if (output.exceeded) {
         FORGE_THROW_EXCEPTION(exceptions::limit, "S3 response exceeds the configured byte limit");
      }
      if (output.failed) {
         FORGE_THROW_EXCEPTION(exceptions::io, "S3 download file write failed");
      }
   }
};

client::impl::impl(asio::compute::executor executor, config options)
    : _executor{std::move(executor)}, _options{std::move(options)} {
   const auto valid_endpoint = [](const std::string& value) {
      if (value.empty() || value.size() > 4096 || value.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
         return false;
      }
      const Aws::Http::URI uri{aws_string(value)};
      return (value.starts_with("https://") || value.starts_with("http://")) && !uri.GetAuthority().empty() &&
             uri.GetAuthority().find('@') == Aws::String::npos && uri.GetQueryString().empty() &&
             (uri.GetPath().empty() || uri.GetPath() == "/") && value.find('#') == std::string::npos;
   };
   if (!_executor.valid() || !valid_endpoint(_options.endpoint) ||
       (!_options.signing_endpoint.empty() && !valid_endpoint(_options.signing_endpoint)) || _options.region.empty() ||
       _options.region.size() > 256 || _options.max_calls == 0 || _options.max_calls > 4096 ||
       _options.max_connections == 0 || _options.max_connections > 1024 || _options.max_memory_bytes == 0 ||
       _options.max_memory_bytes > 1024ULL * 1024 * 1024 || _options.max_object_bytes == 0 ||
       _options.max_object_bytes > 5ULL * 1024 * 1024 * 1024 * 1024 || _options.multipart_threshold == 0 ||
       _options.part_bytes < 5 * 1024 * 1024 || _options.part_bytes > _options.max_memory_bytes ||
       _options.connect_timeout.count() <= 0 || _options.request_timeout.count() <= 0 ||
       _options.operation_timeout.count() <= 0 || _options.connect_timeout > std::chrono::minutes{5} ||
       _options.request_timeout > std::chrono::minutes{10} || _options.operation_timeout > std::chrono::hours{24} ||
       _options.max_presign_lifetime.count() <= 0 || _options.max_presign_lifetime > std::chrono::hours{24 * 7}) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 client configuration is invalid");
   }
   _backend = std::make_unique<backend>(_options);
   _options.identity = {};
}

client::impl::~impl() = default;

client::impl::activity::activity(std::shared_ptr<impl> value, request_options request, bool mutation)
    : owner{std::move(value)}, options{std::move(request)}, mutating{mutation} {
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "S3 client is empty");
   }
   deadline = std::chrono::steady_clock::now() + owner->_options.operation_timeout;
   if (options.deadline) {
      deadline = std::min(deadline, *options.deadline);
   }
   const auto lock = std::scoped_lock{owner->_mutex};
   if (owner->_closed) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "S3 client is stopped");
   }
   check();
   if (owner->_active == owner->_options.max_calls) {
      FORGE_THROW_EXCEPTION(exceptions::busy, "S3 client call limit reached");
   }
   ++owner->_active;
}

client::impl::activity::~activity() {
   std::shared_ptr<boost::asio::steady_timer> wake;
   {
      const auto lock = std::scoped_lock{owner->_mutex};
      if (--owner->_active == 0) {
         wake = owner->_drain;
      }
   }
   if (wake) {
      boost::asio::post(wake->get_executor(), [wake] { wake->expires_at(std::chrono::steady_clock::now()); });
   }
}

bool client::impl::activity::interrupted() const noexcept {
   return options.stop.stop_requested() || worker_stop.stop_requested() || owner->_stop.stop_requested() ||
          std::chrono::steady_clock::now() >= deadline;
}

void client::impl::activity::check() const {
   if (interrupted()) {
      if (mutating && started.load()) {
         FORGE_THROW_EXCEPTION(exceptions::unknown_outcome, "S3 mutation was interrupted after dispatch");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         FORGE_THROW_EXCEPTION(exceptions::deadline, "S3 operation deadline reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::canceled, "S3 operation was canceled");
   }
}

void client::impl::update(credentials identity) {
   const auto lock = std::scoped_lock{_mutex};
   if (_closed) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "S3 client is stopped");
   }
   const auto signing_lock = std::scoped_lock{_backend->signing_mutex};
   _backend->identity->update(std::move(identity));
}

void client::impl::stop() noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      _closed = true;
      if (_backend) {
         _backend->client->DisableRequestProcessing();
         if (_backend->signing != _backend->client) {
            _backend->signing->DisableRequestProcessing();
         }
      }
   }
   _stop.request_stop();
}

boost::asio::awaitable<void> client::impl::drain(std::shared_ptr<impl> owner) {
   if (!owner) {
      co_return;
   }
   owner->stop();
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = std::make_shared<boost::asio::steady_timer>(executor);
   timer->expires_at(std::chrono::steady_clock::time_point::max());
   bool wait;
   {
      const auto lock = std::scoped_lock{owner->_mutex};
      if (owner->_draining) {
         FORGE_THROW_EXCEPTION(exceptions::busy, "S3 shutdown is already being awaited");
      }
      owner->_draining = true;
      wait = owner->_active != 0;
      if (wait) {
         owner->_drain = timer;
      }
   }
   if (wait) {
      boost::system::error_code ignored;
      co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
   }
   auto submitted = owner->_executor.try_submit({"s3.shutdown", {}}, [owner] {
      std::unique_ptr<backend> backend;
      {
         const auto lock = std::scoped_lock{owner->_mutex};
         backend = std::move(owner->_backend);
      }
      backend.reset();
   });
   {
      const auto lock = std::scoped_lock{owner->_mutex};
      owner->_drain.reset();
   }
   if (!submitted) {
      const auto lock = std::scoped_lock{owner->_mutex};
      owner->_draining = false;
      FORGE_THROW_EXCEPTION(exceptions::busy, "S3 SDK shutdown needs one dedicated worker slot");
   }
   try {
      co_await std::move(*submitted).wait();
   } catch (...) {
      const auto lock = std::scoped_lock{owner->_mutex};
      owner->_draining = false;
      detail::backend_call([] { throw; });
   }
   const auto lock = std::scoped_lock{owner->_mutex};
   owner->_draining = false;
}

metadata client::impl::head(const object& target, activity& call) {
   validate_object(target);
   Aws::S3::Model::HeadObjectRequest request;
   set_object(request, target);
   backend::bind(request, call);
   const auto outcome = _backend->client->HeadObject(request);
   backend::check(outcome, call);
   const auto& result = outcome.GetResult();
   if (result.GetContentLength() < 0) {
      FORGE_THROW_EXCEPTION(exceptions::service, "S3 returned a negative object size");
   }
   return describe(result, static_cast<std::uint64_t>(result.GetContentLength()));
}

std::vector<std::byte> client::impl::get(const object& target, const read_options& read, activity& call) {
   validate_object(target);
   validate_range(read, _options.max_memory_bytes);
   auto output = std::make_shared<backend::sink>(read.max_bytes);
   Aws::S3::Model::GetObjectRequest request;
   set_read(request, target, read);
   request.SetResponseStreamFactory([output] { return Aws::New<Aws::IOStream>("forge.s3", output.get()); });
   backend::bind(request, call);
   auto outcome = _backend->client->GetObject(request);
   _backend->check_sink(*output);
   backend::check(outcome, call);
   check_read(outcome.GetResult(), read, output->size);
   return std::move(output->bytes);
}

metadata client::impl::get(const object& target, const std::filesystem::path& destination, const read_options& read,
                           activity& call) {
   validate_object(target);
   validate_range(read, _options.max_object_bytes);
   auto output = std::make_shared<backend::sink>(read.max_bytes, destination);
   Aws::S3::Model::GetObjectRequest request;
   set_read(request, target, read);
   request.SetResponseStreamFactory([output] { return Aws::New<Aws::IOStream>("forge.s3", output.get()); });
   backend::bind(request, call);
   auto outcome = _backend->client->GetObject(request);
   _backend->check_sink(*output);
   backend::check(outcome, call);
   check_read(outcome.GetResult(), read, output->size);
   auto result = describe(outcome.GetResult(), output->size);
   output->publish(destination);
   return result;
}

void client::impl::erase(const object& target, activity& call) {
   validate_object(target);
   Aws::S3::Model::DeleteObjectRequest request;
   set_object(request, target);
   backend::bind(request, call);
   const auto outcome = _backend->client->DeleteObject(request);
   backend::check(outcome, call);
}

signed_url client::impl::presign(const object& target, std::chrono::seconds lifetime, method verb, activity& call) {
   validate_object(target, false);
   if (lifetime.count() <= 0 || lifetime > _options.max_presign_lifetime ||
       (verb != method::get && verb != method::put)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 signing lifetime or method is invalid");
   }
   call.check();
   const auto signing_lock = std::scoped_lock{_backend->signing_mutex};
   const auto now = std::chrono::system_clock::now();
   const auto expires = _backend->identity->expiry(now + lifetime);
   lifetime = std::chrono::duration_cast<std::chrono::seconds>(expires - now);
   if (lifetime.count() <= 0) {
      FORGE_THROW_EXCEPTION(exceptions::denied, "S3 credentials expire before the requested URL can be used");
   }
   auto url = _backend->signing->GeneratePresignedUrl(aws_string(target.bucket), aws_string(target.key),
                                                      verb == method::get ? Aws::Http::HttpMethod::HTTP_GET
                                                                          : Aws::Http::HttpMethod::HTTP_PUT,
                                                      static_cast<std::uint64_t>(lifetime.count()));
   if (url.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::service, "S3 URL signing failed");
   }
   return {crypto::core::secret_string{string(url)}, now + lifetime};
}

multipart client::impl::begin(const object& target, const write_options& write, activity& call) {
   validate_object(target, false);
   validate_write(write);
   Aws::S3::Model::CreateMultipartUploadRequest request;
   set_object(request, target);
   request.SetContentType(aws_string(write.content_type));
   backend::bind(request, call);
   const auto outcome = _backend->client->CreateMultipartUpload(request);
   backend::check(outcome, call);
   multipart result{target, string(outcome.GetResult().GetUploadId())};
   validate_session(result);
   return result;
}

part client::impl::upload(const multipart& session, std::uint32_t number,
                          std::variant<std::vector<std::byte>, std::filesystem::path> input,
                          std::optional<byte_range> range, activity& call) {
   validate_session(session);
   if (number == 0 || number > 10000) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 multipart part number is invalid");
   }
   auto source =
       std::make_shared<backend::source>(std::move(input), range, _options.part_bytes, _options.max_memory_bytes, call);
   if (source->size == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 multipart part is empty");
   }
   auto body = Aws::MakeShared<Aws::IOStream>("forge.s3", source.get());
   Aws::S3::Model::UploadPartRequest request;
   set_object(request, session.target);
   request.SetUploadId(aws_string(session.id));
   request.SetPartNumber(static_cast<int>(number));
   request.SetContentLength(static_cast<long long>(source->size));
   request.SetBody(body);
   backend::bind(request, call);
   const auto outcome = _backend->client->UploadPart(request);
   backend::check(outcome, call);
   return {number, source->size, string(outcome.GetResult().GetETag())};
}

part_page client::impl::parts(const multipart& session, std::uint32_t after, std::size_t limit, activity& call) {
   validate_session(session);
   if (after > 10000 || limit == 0 || limit > 1000) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 part listing bounds are invalid");
   }
   Aws::S3::Model::ListPartsRequest request;
   set_object(request, session.target);
   request.SetUploadId(aws_string(session.id));
   request.SetPartNumberMarker(static_cast<int>(after));
   request.SetMaxParts(static_cast<int>(limit));
   backend::bind(request, call);
   const auto outcome = _backend->client->ListParts(request);
   backend::check(outcome, call);
   part_page page;
   for (const auto& value : outcome.GetResult().GetParts()) {
      if (value.GetPartNumber() <= static_cast<int>(after) || value.GetPartNumber() > 10000 || value.GetSize() < 0 ||
          page.parts.size() >= limit ||
          (!page.parts.empty() && value.GetPartNumber() <= static_cast<int>(page.parts.back().number))) {
         FORGE_THROW_EXCEPTION(exceptions::service, "S3 returned an invalid part listing");
      }
      page.parts.push_back({static_cast<std::uint32_t>(value.GetPartNumber()),
                            static_cast<std::uint64_t>(value.GetSize()), string(value.GetETag())});
   }
   if (outcome.GetResult().GetIsTruncated()) {
      const auto next = outcome.GetResult().GetNextPartNumberMarker();
      if (next <= static_cast<int>(after) || next > 10000) {
         FORGE_THROW_EXCEPTION(exceptions::service, "S3 part listing cursor did not advance");
      }
      page.next = static_cast<std::uint32_t>(next);
   }
   return page;
}

metadata client::impl::complete(const multipart& session, std::vector<part> parts, const write_options& conditions,
                                activity& call) {
   validate_session(session);
   validate_write(conditions);
   if (parts.empty() || parts.size() > 10000) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 completion part count is invalid");
   }
   std::sort(parts.begin(), parts.end(),
             [](const part& left, const part& right) { return left.number < right.number; });
   Aws::S3::Model::CompletedMultipartUpload upload;
   std::uint64_t size = 0;
   for (std::size_t index = 0; index != parts.size(); ++index) {
      const auto& value = parts[index];
      if (value.number != index + 1 || value.etag.empty() || value.etag.size() > 1024 ||
          value.size > _options.part_bytes || value.size > _options.max_object_bytes - size ||
          (index + 1 < parts.size() && value.size < 5 * 1024 * 1024)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "S3 completion parts are invalid or exceed limits");
      }
      size += value.size;
      Aws::S3::Model::CompletedPart native;
      native.SetPartNumber(static_cast<int>(value.number));
      native.SetETag(aws_string(value.etag));
      upload.AddParts(std::move(native));
   }
   Aws::S3::Model::CompleteMultipartUploadRequest request;
   set_object(request, session.target);
   request.SetUploadId(aws_string(session.id));
   request.SetMultipartUpload(std::move(upload));
   set_conditions(request, conditions);
   backend::bind(request, call);
   const auto outcome = _backend->client->CompleteMultipartUpload(request);
   backend::check(outcome, call);
   return describe(outcome.GetResult(), size);
}

void client::impl::abort(const multipart& session, activity& call) {
   validate_session(session);
   Aws::S3::Model::AbortMultipartUploadRequest request;
   set_object(request, session.target);
   request.SetUploadId(aws_string(session.id));
   backend::bind(request, call);
   const auto outcome = _backend->client->AbortMultipartUpload(request);
   if (!outcome.IsSuccess() && static_cast<unsigned>(outcome.GetError().GetResponseCode()) == 404) {
      return;
   }
   backend::check(outcome, call);
}

metadata client::impl::put(const object& target, std::variant<std::vector<std::byte>, std::filesystem::path> input,
                           const write_options& write, activity& call) {
   validate_object(target, false);
   validate_write(write);
   auto source = std::make_shared<backend::source>(std::move(input), std::nullopt, _options.max_object_bytes,
                                                   _options.max_memory_bytes, call);
   if (source->size < _options.multipart_threshold) {
      if (source->size > 5ULL * 1024 * 1024 * 1024) {
         FORGE_THROW_EXCEPTION(exceptions::limit, "S3 single upload exceeds 5 GiB");
      }
      auto body = Aws::MakeShared<Aws::IOStream>("forge.s3", source.get());
      Aws::S3::Model::PutObjectRequest request;
      set_object(request, target);
      set_conditions(request, write);
      request.SetContentType(aws_string(write.content_type));
      request.SetContentLength(static_cast<long long>(source->size));
      request.SetBody(body);
      backend::bind(request, call);
      const auto outcome = _backend->client->PutObject(request);
      backend::check(outcome, call);
      auto result = describe(outcome.GetResult(), source->size);
      result.content_type = write.content_type;
      return result;
   }
   if ((source->size + _options.part_bytes - 1) / _options.part_bytes > 10000) {
      FORGE_THROW_EXCEPTION(exceptions::limit,
                            "S3 automatic multipart exceeds 10000 parts; configure a larger part size");
   }
   auto session = begin(target, write, call);
   auto completed = std::vector<part>{};
   auto completing = false;
   try {
      auto stream = std::istream{source.get()};
      std::uint64_t remaining = source->size;
      while (remaining != 0) {
         call.check();
         auto bytes =
             std::vector<std::byte>(static_cast<std::size_t>(std::min<std::uint64_t>(_options.part_bytes, remaining)));
         stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
         if (static_cast<std::size_t>(stream.gcount()) != bytes.size()) {
            call.check();
            FORGE_THROW_EXCEPTION(exceptions::io, "S3 multipart source ended early");
         }
         remaining -= bytes.size();
         completed.push_back(
             upload(session, static_cast<std::uint32_t>(completed.size() + 1), std::move(bytes), {}, call));
      }
      completing = true;
      auto result = complete(session, std::move(completed), write, call);
      result.content_type = write.content_type;
      return result;
   } catch (...) {
      const auto original = std::current_exception();
      auto cleanup_attempted = false;
      auto cleanup_confirmed = false;
      if (!completing) {
         try {
            Aws::S3::Model::AbortMultipartUploadRequest cleanup;
            set_object(cleanup, session.target);
            cleanup.SetUploadId(aws_string(session.id));
            // One finite best-effort cleanup call. Failed cleanup needs bucket lifecycle/recovery.
            auto output = std::make_shared<backend::response>(call.response_exceeded);
            cleanup.SetResponseStreamFactory([output] { return Aws::New<Aws::IOStream>("forge.s3", output.get()); });
            cleanup_attempted = true;
            cleanup_confirmed = _backend->client->AbortMultipartUpload(cleanup).IsSuccess();
         } catch (...) {
         }
      }
      try {
         return detail::backend_call([&]() -> metadata { std::rethrow_exception(original); }, &call.started);
      } catch (forge::exceptions::base& error) {
         error.append_context("multipart session needs reconciliation",
                              {forge::exceptions::ctx("upload_id", session.id),
                               forge::exceptions::ctx("bucket", session.target.bucket),
                               forge::exceptions::ctx("key", session.target.key),
                               forge::exceptions::ctx("cleanup_attempted", cleanup_attempted),
                               forge::exceptions::ctx("cleanup_confirmed", cleanup_confirmed),
                               forge::exceptions::ctx("completion_attempted", completing)});
         throw;
      }
   }
}

} // namespace forge::net::s3
