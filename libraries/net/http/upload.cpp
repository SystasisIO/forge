module;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/asio/awaitable.hpp>

#include <forge/exceptions/macros.hpp>

module forge.net.http.upload;

import forge.net.http.exceptions;

#include "details/multipart_framing.hxx"
#include "details/multipart_stream_source.hxx"

namespace forge::net::http {

struct upload_spool::state {
   explicit state(std::filesystem::path path_value) : path(std::move(path_value)) {}

   ~state() {
      if (cleanup) {
         std::error_code ignored;
         std::filesystem::remove(path, ignored);
      }
   }

   std::filesystem::path path;
   std::uint64_t size = 0;
   bool cleanup = true;
};

namespace {

std::filesystem::path effective_spool_directory(const upload_options& options) {
   auto directory = options.spool_directory.empty() ? std::filesystem::temp_directory_path()
                                                    : options.spool_directory;
   std::filesystem::create_directories(directory);
   if (!std::filesystem::is_directory(directory)) {
      throw exceptions::bad_request{"upload spool directory is not a directory"};
   }
   return directory;
}

std::string text_from_bytes(const std::vector<std::byte>& bytes) {
   return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string read_file_text(const std::filesystem::path& path) {
   auto input = std::ifstream{path, std::ios::binary};
   if (!input) {
      throw exceptions::internal{"failed to read upload spool"};
   }
   auto output = std::ostringstream{};
   output << input.rdbuf();
   return output.str();
}

void write_all(int fd, const std::byte* data, std::size_t size) {
   auto written = std::size_t{0};
   while (written != size) {
      const auto result = ::write(fd, data + written, size - written);
      if (result < 0) {
         if (errno == EINTR) {
            continue;
         }
         throw exceptions::internal{"failed to write upload spool"};
      }
      written += static_cast<std::size_t>(result);
   }
}

std::string trim_copy(std::string_view value) {
   while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
   }
   while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
   }
   return std::string{value};
}

std::string lower_copy(std::string_view value) {
   auto output = std::string{value};
   std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
   });
   return output;
}

std::string unquote(std::string value)
{
   if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
   {
      auto output = std::size_t{};
      for (auto index = std::size_t{1}; index + 1 < value.size(); ++index)
      {
         if (value[index] == '\\')
         {
            if (++index + 1 >= value.size())
            {
               FORGE_THROW_EXCEPTION(exceptions::bad_request, "Multipart parameter has an incomplete quoted pair");
            }
         }

         value[output++] = value[index];
      }

      value.resize(output);
   }

   return value;
}

std::vector<std::string_view> split(std::string_view value, char delimiter) {
   auto parts = std::vector<std::string_view>{};
   auto start = std::size_t{0};
   while (start <= value.size()) {
      const auto found = value.find(delimiter, start);
      const auto end = found == std::string_view::npos ? value.size() : found;
      parts.push_back(value.substr(start, end - start));
      if (found == std::string_view::npos) {
         break;
      }
      start = found + 1U;
   }
   return parts;
}

std::optional<std::string> header_value(const std::vector<std::pair<std::string, std::string>>& headers,
                                        std::string_view name) {
   const auto needle = lower_copy(name);
   for (const auto& [key, value] : headers) {
      if (lower_copy(key) == needle) {
         return value;
      }
   }
   return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> parse_headers(std::string_view value) {
   auto headers = std::vector<std::pair<std::string, std::string>>{};
   auto start = std::size_t{0};
   while (start < value.size()) {
      const auto end = value.find("\r\n", start);
      auto line = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
      const auto colon = line.find(':');
      if (colon == std::string_view::npos) {
         throw exceptions::bad_request{"malformed multipart header"};
      }
      headers.emplace_back(trim_copy(line.substr(0, colon)), trim_copy(line.substr(colon + 1U)));
      if (end == std::string_view::npos) {
         break;
      }
      start = end + 2U;
   }
   return headers;
}

struct disposition {
   std::string name;
   std::optional<std::string> filename;
};

disposition parse_content_disposition(const std::vector<std::pair<std::string, std::string>>& headers) {
   const auto value = header_value(headers, "content-disposition");
   if (!value.has_value()) {
      throw exceptions::bad_request{"multipart part is missing Content-Disposition"};
   }

   auto result = disposition{};
   auto parts = split(*value, ';');
   if (parts.empty() || lower_copy(trim_copy(parts.front())) != "form-data") {
      throw exceptions::bad_request{"multipart part is not form-data"};
   }

   for (auto index = std::size_t{1}; index < parts.size(); ++index) {
      const auto parameter = trim_copy(parts[index]);
      const auto equals = parameter.find('=');
      if (equals == std::string::npos) {
         continue;
      }
      const auto key = lower_copy(trim_copy(std::string_view{parameter}.substr(0, equals)));
      auto parameter_value = unquote(trim_copy(std::string_view{parameter}.substr(equals + 1U)));
      if (key == "name") {
         result.name = std::move(parameter_value);
      } else if (key == "filename") {
         result.filename = std::move(parameter_value);
      }
   }

   if (result.name.empty()) {
      throw exceptions::bad_request{"multipart part is missing name"};
   }
   return result;
}

} // namespace

class upload_spool_writer {
 public:
   explicit upload_spool_writer(const upload_options& options) {
      const auto directory = effective_spool_directory(options);
      const auto prefix = options.spool_prefix.empty() ? std::string{"forge-http-upload-"} : options.spool_prefix;
      const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
      for (auto attempt = 0; attempt != 64; ++attempt) {
         auto candidate = directory / (prefix + std::to_string(::getpid()) + "-" + std::to_string(seed) + "-" +
                                      std::to_string(attempt) + ".tmp");
         const auto fd = ::open(candidate.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
         if (fd >= 0) {
            fd_ = fd;
            state_ = std::make_shared<upload_spool::state>(std::move(candidate));
            return;
         }
         if (errno != EEXIST && errno != EINTR) {
            throw exceptions::internal{"failed to create upload spool"};
         }
      }
      throw exceptions::internal{"failed to allocate upload spool"};
   }

   ~upload_spool_writer() {
      if (fd_ >= 0) {
         ::close(fd_);
      }
   }

   upload_spool_writer(const upload_spool_writer&) = delete;
   upload_spool_writer& operator=(const upload_spool_writer&) = delete;

   void write(const std::vector<std::byte>& bytes) {
      write(bytes.data(), bytes.size());
   }

   void write(const std::byte* data, std::size_t size) {
      if (size == 0) {
         return;
      }
      write_all(fd_, data, size);
      state_->size += size;
   }

   upload_spool finish() {
      if (fd_ >= 0) {
         ::close(fd_);
         fd_ = -1;
      }
      return upload_spool{state_};
   }

 private:
   int fd_ = -1;
   std::shared_ptr<upload_spool::state> state_;
};

namespace {

void append_text_bytes(std::vector<std::byte>& target, std::string_view text) {
   if (text.empty()) {
      return;
   }
   const auto old_size = target.size();
   target.resize(old_size + text.size());
   std::memcpy(target.data() + old_size, text.data(), text.size());
}

struct multipart_delimiter {
   std::size_t position = 0;
   bool closing = false;
};

std::optional<multipart_delimiter> find_next_multipart_delimiter(std::string_view body, std::string_view delimiter,
                                                                 std::size_t offset, bool eof) {
   const auto marker = std::string{"\r\n"} + std::string{delimiter};
   while (true) {
      const auto candidate = body.find(marker, offset);
      if (candidate == std::string_view::npos) {
         return std::nullopt;
      }

      const auto suffix = candidate + marker.size();
      if (!eof && suffix >= body.size()) {
         return std::nullopt;
      }
      if (body.substr(suffix, 2U) == "\r\n") {
         return multipart_delimiter{.position = candidate, .closing = false};
      }
      if (body.substr(suffix, 2U) == "--") {
         const auto after_close = suffix + 2U;
         if (body.substr(after_close, 2U) == "\r\n" || (eof && after_close == body.size())) {
            return multipart_delimiter{.position = candidate, .closing = true};
         }
         if (!eof && after_close >= body.size()) {
            return std::nullopt;
         }
      }
      offset = candidate + 1U;
   }
}

class multipart_part_builder {
 public:
   multipart_part_builder(disposition meta, std::string content_type,
                          std::vector<std::pair<std::string, std::string>> headers, const upload_options& options)
      : name_(std::move(meta.name)), filename_(std::move(meta.filename)), content_type_(std::move(content_type)),
        headers_(std::move(headers)), options_(options) {}

   void append(std::string_view bytes) {
      size_ += bytes.size();
      if (filename_.has_value() && size_ > options_.max_file_bytes) {
         throw exceptions::payload_too_large{"multipart file exceeds upload limit"};
      }
      if (!filename_.has_value() && size_ > options_.max_field_bytes) {
         throw exceptions::payload_too_large{"multipart field exceeds upload limit"};
      }
      if (bytes.empty()) {
         return;
      }

      if (!writer_.has_value() && memory_.size() + bytes.size() <= options_.memory_threshold_bytes) {
         append_text_bytes(memory_, bytes);
         return;
      }

      if (!writer_.has_value()) {
         writer_.emplace(options_);
         writer_->write(memory_);
         memory_.clear();
      }
      writer_->write(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
   }

   upload_part finish() {
      auto part = upload_part{
         .name = std::move(name_),
         .filename = std::move(filename_),
         .content_type = std::move(content_type_),
         .headers = std::move(headers_),
         .memory = std::move(memory_),
         .spool = std::nullopt,
         .size = size_,
      };
      if (writer_.has_value()) {
         part.memory.clear();
         part.spool = writer_->finish();
      }
      return part;
   }

 private:
   std::string name_;
   std::optional<std::string> filename_;
   std::string content_type_;
   std::vector<std::pair<std::string, std::string>> headers_;
   const upload_options& options_;
   std::vector<std::byte> memory_;
   std::optional<upload_spool_writer> writer_;
   std::uint64_t size_ = 0;
};

class multipart_stream_parser {
 public:
   multipart_stream_parser(std::string boundary, const upload_options& options)
      : delimiter_(std::string{"--"} + std::move(boundary)), options_(options) {}

   boost::asio::awaitable<multipart_form> parse(body_reader& body) {
      while (auto chunk = co_await body.async_read()) {
         total_ += chunk->bytes.size();
         if (total_ > options_.max_total_bytes) {
            throw exceptions::payload_too_large{"upload exceeds configured total limit"};
         }
         buffer_.append(reinterpret_cast<const char*>(chunk->bytes.data()), chunk->bytes.size());
         process(false);
      }

      process(true);
      if (state_ != state::done) {
         throw exceptions::bad_request{"multipart closing boundary is missing"};
      }
      co_return std::move(form_);
   }

 private:
   enum class state {
      first_boundary,
      headers,
      content,
      done,
   };

   void process(bool eof) {
      while (true) {
         switch (state_) {
         case state::first_boundary:
            if (!consume_first_boundary(eof)) {
               return;
            }
            break;
         case state::headers:
            if (!consume_headers()) {
               return;
            }
            break;
         case state::content:
            if (!consume_content(eof)) {
               return;
            }
            break;
         case state::done:
            if (!buffer_.empty()) {
               throw exceptions::bad_request{"unexpected multipart trailer"};
            }
            return;
         }
      }
   }

   bool consume_first_boundary(bool eof) {
      if (buffer_.size() < delimiter_.size()) {
         if (eof) {
            throw exceptions::bad_request{"multipart body does not start with boundary"};
         }
         return false;
      }
      if (!std::string_view{buffer_}.starts_with(delimiter_)) {
         throw exceptions::bad_request{"multipart body does not start with boundary"};
      }
      if (buffer_.size() < delimiter_.size() + 2U) {
         if (eof) {
            throw exceptions::bad_request{"malformed multipart boundary"};
         }
         return false;
      }

      const auto suffix = std::string_view{buffer_}.substr(delimiter_.size(), 2U);
      if (suffix == "--") {
         buffer_.erase(0, delimiter_.size() + 2U);
         consume_optional_closing_crlf(eof);
         state_ = state::done;
         return true;
      }
      if (suffix != "\r\n") {
         throw exceptions::bad_request{"malformed multipart boundary"};
      }
      buffer_.erase(0, delimiter_.size() + 2U);
      state_ = state::headers;
      return true;
   }

   void consume_optional_closing_crlf(bool eof) {
      if (buffer_.empty()) {
         return;
      }
      if (buffer_.starts_with("\r\n")) {
         buffer_.erase(0, 2U);
      }
      if (eof && !buffer_.empty()) {
         throw exceptions::bad_request{"unexpected multipart trailer"};
      }
   }

   bool consume_headers() {
      const auto header_end = buffer_.find("\r\n\r\n");
      if (header_end == std::string::npos) {
         if (buffer_.size() > options_.max_part_header_bytes + 4U) {
            throw exceptions::payload_too_large{"multipart part headers exceed upload limit"};
         }
         return false;
      }
      if (header_end > options_.max_part_header_bytes) {
         throw exceptions::payload_too_large{"multipart part headers exceed upload limit"};
      }

      auto headers = parse_headers(std::string_view{buffer_}.substr(0, header_end));
      if (headers.size() > options_.max_part_headers) {
         throw exceptions::payload_too_large{"multipart part header count exceeds upload limit"};
      }
      auto meta = parse_content_disposition(headers);
      auto content_type = header_value(headers, "content-type").value_or("application/octet-stream");
      current_.emplace(std::move(meta), std::move(content_type), std::move(headers), options_);
      buffer_.erase(0, header_end + 4U);
      state_ = state::content;
      return true;
   }

   bool consume_content(bool eof) {
      auto delimiter = find_next_multipart_delimiter(buffer_, delimiter_, 0, eof);
      if (!delimiter.has_value()) {
         if (eof) {
            throw exceptions::bad_request{"multipart closing boundary is missing"};
         }
         flush_safe_content_prefix();
         return false;
      }

      current_->append(std::string_view{buffer_}.substr(0, delimiter->position));
      auto part = current_->finish();
      record_part(std::move(part));
      current_.reset();

      buffer_.erase(0, delimiter->position + 2U + delimiter_.size());
      if (delimiter->closing) {
         if (!buffer_.starts_with("--")) {
            throw exceptions::bad_request{"malformed multipart boundary"};
         }
         buffer_.erase(0, 2U);
         consume_optional_closing_crlf(eof);
         state_ = state::done;
      } else {
         if (!buffer_.starts_with("\r\n")) {
            throw exceptions::bad_request{"malformed multipart boundary"};
         }
         buffer_.erase(0, 2U);
         state_ = state::headers;
      }
      return true;
   }

   void record_part(upload_part part) {
      if (parts_ >= options_.max_parts) {
         throw exceptions::payload_too_large{"multipart part count exceeds upload limit"};
      }
      ++parts_;
      if (part.filename.has_value()) {
         if (files_ >= options_.max_files) {
            throw exceptions::payload_too_large{"multipart file count exceeds upload limit"};
         }
         ++files_;
         form_.files.push_back(part);
      } else {
         if (fields_ >= options_.max_fields) {
            throw exceptions::payload_too_large{"multipart field count exceeds upload limit"};
         }
         ++fields_;
      }
      form_.parts.push_back(std::move(part));
   }

   void flush_safe_content_prefix() {
      const auto tail_size = delimiter_.size() + 6U;
      if (buffer_.size() <= tail_size) {
         return;
      }
      const auto flush_size = buffer_.size() - tail_size;
      current_->append(std::string_view{buffer_}.substr(0, flush_size));
      buffer_.erase(0, flush_size);
   }

   std::string delimiter_;
   const upload_options& options_;
   multipart_form form_;
   std::string buffer_;
   std::optional<multipart_part_builder> current_;
   std::uint64_t total_ = 0;
   std::uint64_t parts_ = 0;
   std::uint64_t files_ = 0;
   std::uint64_t fields_ = 0;
   state state_ = state::first_boundary;
};

} // namespace

upload_spool::upload_spool(std::shared_ptr<state> state_value) : state_(std::move(state_value)) {}

bool upload_spool::valid() const noexcept {
   return static_cast<bool>(state_);
}

const std::filesystem::path& upload_spool::path() const {
   if (!state_) {
      throw exceptions::internal{"upload spool is not valid"};
   }
   return state_->path;
}

std::uint64_t upload_spool::size() const noexcept {
   return state_ ? state_->size : 0;
}

void upload_spool::release() noexcept {
   if (state_) {
      state_->cleanup = false;
   }
}

bool upload_part::in_memory() const noexcept {
   return !spool.has_value();
}

std::string upload_part::text() const {
   if (spool.has_value()) {
      return read_file_text(spool->path());
   }
   return text_from_bytes(memory);
}

std::optional<std::string> upload_part::safe_filename() const {
   if (!filename.has_value()) {
      return std::nullopt;
   }
   return sanitize_upload_filename(*filename);
}

std::optional<std::string> multipart_form::field(std::string_view name) const {
   for (const auto& part : parts) {
      if (!part.filename.has_value() && part.name == name) {
         return part.text();
      }
   }
   return std::nullopt;
}

upload_reader::upload_reader(body_reader body, upload_options options)
    : body_(std::move(body)), options_(std::move(options)) {}

boost::asio::awaitable<upload_part> upload_reader::async_read() {
   auto memory = std::vector<std::byte>{};
   auto total = std::uint64_t{0};
   auto writer = std::optional<upload_spool_writer>{};

   while (auto chunk = co_await body_.async_read()) {
      total += chunk->bytes.size();
      if (total > options_.max_total_bytes || total > options_.max_file_bytes) {
         throw exceptions::payload_too_large{"upload exceeds configured limit"};
      }

      if (!writer.has_value() && memory.size() + chunk->bytes.size() <= options_.memory_threshold_bytes) {
         memory.insert(memory.end(), chunk->bytes.begin(), chunk->bytes.end());
         continue;
      }

      if (!writer.has_value()) {
         writer.emplace(options_);
         writer->write(memory);
         memory.clear();
      }
      writer->write(chunk->bytes);
   }

   if (writer.has_value()) {
      co_return upload_part{.memory = {}, .spool = writer->finish(), .size = total};
   }

   co_return upload_part{.memory = std::move(memory), .spool = std::nullopt, .size = total};
}

boost::asio::awaitable<multipart_form> upload_reader::async_read_multipart(std::string_view content_type) {
   const auto boundary = multipart_boundary(content_type);
   if (!boundary.has_value()) {
      throw exceptions::bad_request{"multipart boundary is missing"};
   }

   auto parser = multipart_stream_parser{*boundary, options_};
   co_return co_await parser.parse(body_);
}

std::optional<std::string> multipart_boundary(std::string_view content_type) {
   for (auto piece : split(content_type, ';')) {
      auto parameter = trim_copy(piece);
      const auto equals = parameter.find('=');
      if (equals == std::string::npos) {
         continue;
      }
      const auto key = lower_copy(trim_copy(std::string_view{parameter}.substr(0, equals)));
      if (key != "boundary") {
         continue;
      }
      auto value = unquote(trim_copy(std::string_view{parameter}.substr(equals + 1U)));
      if (value.empty() || value.find('\r') != std::string::npos || value.find('\n') != std::string::npos) {
         throw exceptions::bad_request{"multipart boundary is invalid"};
      }
      return value;
   }
   return std::nullopt;
}

std::optional<std::string> sanitize_upload_filename(std::string_view filename) {
   auto basename = filename;
   if (const auto slash = basename.find_last_of("/\\"); slash != std::string_view::npos) {
      basename.remove_prefix(slash + 1U);
   }
   if (basename.empty() || basename == "." || basename == "..") {
      return std::nullopt;
   }

   auto output = std::string{};
   output.reserve(basename.size());
   for (const auto value : basename) {
      const auto c = static_cast<unsigned char>(value);
      if (std::isalnum(c) || value == '.' || value == '_' || value == '-') {
         output.push_back(value);
      } else {
         output.push_back('_');
      }
   }

   if (output.empty() || output == "." || output == "..") {
      return std::nullopt;
   }
   return output;
}

bool multipart_body_contains_boundary(const std::vector<multipart_writer_part>& parts, std::string_view boundary) {
   for (const auto& part : parts) {
      if (part.body.find(boundary) != std::string::npos) {
         return true;
      }
   }
   return false;
}

std::string generate_multipart_boundary(const std::vector<multipart_writer_part>& parts) {
   for (;;) {
      auto boundary = multipart_framing::boundary();
      if (!multipart_body_contains_boundary(parts, boundary)) {
         return boundary;
      }
   }
}

multipart_writer_result write_multipart_form(std::vector<multipart_writer_part> parts) {
   const auto boundary = generate_multipart_boundary(parts);
   auto body = std::string{};

   for (const auto& part : parts) {
      body += multipart_framing::header(boundary, part.name, part.filename, part.content_type);
      body += part.body;
      body += "\r\n";
   }

   body += "--";
   body += boundary;
   body += "--\r\n";
   return multipart_writer_result{
      .content_type = std::string{"multipart/form-data; boundary="} + boundary,
      .body = std::move(body),
   };
}

multipart_stream_result write_multipart_stream(std::vector<multipart_stream_part> parts,
                                               multipart_stream_options options)
{
   auto boundary = multipart_framing::boundary();
   auto source = std::make_shared<multipart_stream_source>(std::move(parts), boundary, options);
   return {.content_type = "multipart/form-data; boundary=" + boundary, .body = body_reader{std::move(source)}};
}

} // namespace forge::net::http
