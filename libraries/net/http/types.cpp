module;

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.http.types;

namespace forge::net::http {
namespace {

std::vector<header_entry>::const_iterator find_header(const std::vector<header_entry>& headers,
                                                      std::string_view name) noexcept {
   return std::find_if(headers.begin(), headers.end(),
                       [&](const header_entry& entry) { return header_name_equal(entry.name, name); });
}

void set_header(std::vector<header_entry>& headers, std::string_view name, std::string value) {
   headers.erase(std::remove_if(headers.begin(), headers.end(),
                                [&](const header_entry& entry) { return header_name_equal(entry.name, name); }),
                 headers.end());
   headers.push_back(header_entry{.name = std::string{name}, .text = std::move(value)});
}

bool response_status_allows_payload(status value) noexcept {
   return value != status::no_content && value != status::not_modified;
}

} // namespace

bool header_name_equal(std::string_view left, std::string_view right) noexcept {
   if (left.size() != right.size()) {
      return false;
   }
   for (auto index = std::size_t{0}; index != left.size(); ++index) {
      const auto lhs = static_cast<unsigned char>(left[index]);
      const auto rhs = static_cast<unsigned char>(right[index]);
      if (std::tolower(lhs) != std::tolower(rhs)) {
         return false;
      }
   }
   return true;
}

std::optional<std::string_view> find_header(std::span<const header_entry> headers, std::string_view name) noexcept {
   const auto found = std::find_if(headers.begin(), headers.end(),
                                   [&](const header_entry& entry) { return header_name_equal(entry.name, name); });
   if (found == headers.end()) {
      return std::nullopt;
   }
   return std::string_view{found->text};
}

void set_header(std::vector<header_entry>& headers, std::string name, std::string value) {
   headers.erase(std::remove_if(headers.begin(), headers.end(),
                                [&](const header_entry& entry) { return header_name_equal(entry.name, name); }),
                 headers.end());
   headers.push_back(header_entry{.name = std::move(name), .text = std::move(value)});
}

struct request::state {
   forge::net::http::method method_value = forge::net::http::method::get;
   std::string target_value = "/";
   unsigned version_value = 11;
   bool keep_alive_value = true;
   std::string body_value;
   std::vector<header_entry> headers;
};

struct response::state {
   status status_value = status::ok;
   unsigned version_value = 11;
   bool keep_alive_value = true;
   std::string body_value;
   std::vector<header_entry> headers;
};

struct endpoint_state {
   forge::net::http::request request_value;
   forge::net::http::response response_value;
};

const header_entry& header_iterator::operator*() const noexcept {
   return *value_;
}

const header_entry* header_iterator::operator->() const noexcept {
   return &*value_;
}

bool operator==(const header_iterator& left, const header_iterator& right) noexcept {
   return left.value_.has_value() == right.value_.has_value() &&
          (!left.value_.has_value() ||
           (left.value_->name == right.value_->name && left.value_->text == right.value_->text));
}

header_iterator::header_iterator(std::optional<header_entry> value) : value_(std::move(value)) {}

std::string_view field_name(field value) noexcept {
   switch (value) {
   case field::accept:
      return "Accept";
   case field::accept_ranges:
      return "Accept-Ranges";
   case field::authorization:
      return "Authorization";
   case field::connection:
      return "Connection";
   case field::content_length:
      return "Content-Length";
   case field::content_range:
      return "Content-Range";
   case field::content_type:
      return "Content-Type";
   case field::cookie:
      return "Cookie";
   case field::etag:
      return "ETag";
   case field::expect:
      return "Expect";
   case field::host:
      return "Host";
   case field::if_modified_since:
      return "If-Modified-Since";
   case field::if_none_match:
      return "If-None-Match";
   case field::if_range:
      return "If-Range";
   case field::last_modified:
      return "Last-Modified";
   case field::range:
      return "Range";
   case field::retry_after:
      return "Retry-After";
   case field::server:
      return "Server";
   case field::transfer_encoding:
      return "Transfer-Encoding";
   case field::user_agent:
      return "User-Agent";
   }
   return {};
}

std::ostream& operator<<(std::ostream& stream, method value) {
   switch (value) {
   case method::delete_:
      return stream << "DELETE";
   case method::get:
      return stream << "GET";
   case method::head:
      return stream << "HEAD";
   case method::options:
      return stream << "OPTIONS";
   case method::patch:
      return stream << "PATCH";
   case method::post:
      return stream << "POST";
   case method::put:
      return stream << "PUT";
   case method::unknown:
      return stream << "UNKNOWN";
   }
   return stream << "UNKNOWN";
}

std::ostream& operator<<(std::ostream& stream, status value) {
   return stream << static_cast<unsigned>(value);
}

request::request() : state_(std::make_shared<state>()) {}

request::request(forge::net::http::method method_value, std::string target_value, unsigned version_value) : request() {
   method(method_value);
   target(std::move(target_value));
   version(version_value);
}

request::~request() = default;
request::request(const request& other) : state_(std::make_shared<state>(*other.state_)) {}
request::request(request&&) noexcept = default;
request& request::operator=(const request& other) {
   if (this != &other) {
      state_ = std::make_shared<state>(*other.state_);
   }
   return *this;
}
request& request::operator=(request&&) noexcept = default;

forge::net::http::method request::method() const noexcept {
   return state_->method_value;
}

void request::method(forge::net::http::method value) noexcept {
   state_->method_value = value;
}

std::string_view request::target() const noexcept {
   return state_->target_value;
}

void request::target(std::string value) {
   state_->target_value = std::move(value);
}

void request::target(std::string_view value) {
   state_->target_value.assign(value);
}

void request::target(const char* value) {
   target(std::string_view{value});
}

unsigned request::version() const noexcept {
   return state_->version_value;
}

void request::version(unsigned value) noexcept {
   state_->version_value = value;
}

bool request::keep_alive() const noexcept {
   return state_->keep_alive_value;
}

void request::keep_alive(bool value) noexcept {
   state_->keep_alive_value = value;
}

std::string& request::body() noexcept {
   return state_->body_value;
}

const std::string& request::body() const noexcept {
   return state_->body_value;
}

void request::set(field name, std::string_view value) {
   set(field_name(name), value);
}

void request::set(std::string_view name, std::string_view value) {
   set_header(state_->headers, name, std::string{value});
}

void request::insert(std::string_view name, std::string_view value) {
   state_->headers.push_back(header_entry{.name = std::string{name}, .text = std::string{value}});
}

void request::erase(field name) {
   erase(field_name(name));
}

void request::erase(std::string_view name) {
   state_->headers.erase(std::remove_if(state_->headers.begin(), state_->headers.end(),
                                        [&](const header_entry& entry) { return header_name_equal(entry.name, name); }),
                         state_->headers.end());
}

header_iterator request::find(field name) const {
   return find(field_name(name));
}

header_iterator request::find(std::string_view name) const {
   const auto found = find_header(state_->headers, name);
   if (found == state_->headers.end()) {
      return end();
   }
   return header_iterator{*found};
}

header_iterator request::end() const noexcept {
   return header_iterator{};
}

std::string request::operator[](field name) const {
   return (*this)[field_name(name)];
}

std::string request::operator[](std::string_view name) const {
   if (const auto value = header(name)) {
      return std::string{*value};
   }
   return {};
}

std::optional<std::string_view> request::header(std::string_view name) const {
   const auto found = find_header(state_->headers, name);
   if (found == state_->headers.end()) {
      return std::nullopt;
   }
   return std::string_view{found->text};
}

std::vector<header_entry> request::headers() const {
   return state_->headers;
}

void request::prepare_payload() {
   set(field::content_length, std::to_string(state_->body_value.size()));
}

response::response() : state_(std::make_shared<state>()) {}

response::response(status status_value, unsigned version_value) : response() {
   result(status_value);
   version(version_value);
}

response::~response() = default;
response::response(const response& other) : state_(std::make_shared<state>(*other.state_)) {}
response::response(response&&) noexcept = default;
response& response::operator=(const response& other) {
   if (this != &other) {
      state_ = std::make_shared<state>(*other.state_);
   }
   return *this;
}
response& response::operator=(response&&) noexcept = default;

status response::result() const noexcept {
   return state_->status_value;
}

void response::result(status value) noexcept {
   state_->status_value = value;
}

unsigned response::result_int() const noexcept {
   return static_cast<unsigned>(state_->status_value);
}

unsigned response::version() const noexcept {
   return state_->version_value;
}

void response::version(unsigned value) noexcept {
   state_->version_value = value;
}

bool response::keep_alive() const noexcept {
   return state_->keep_alive_value;
}

void response::keep_alive(bool value) noexcept {
   state_->keep_alive_value = value;
}

std::string& response::body() noexcept {
   return state_->body_value;
}

const std::string& response::body() const noexcept {
   return state_->body_value;
}

void response::set(field name, std::string_view value) {
   set(field_name(name), value);
}

void response::set(std::string_view name, std::string_view value) {
   set_header(state_->headers, name, std::string{value});
}

void response::insert(std::string_view name, std::string_view value) {
   state_->headers.push_back(header_entry{.name = std::string{name}, .text = std::string{value}});
}

void response::erase(field name) {
   erase(field_name(name));
}

void response::erase(std::string_view name) {
   state_->headers.erase(std::remove_if(state_->headers.begin(), state_->headers.end(),
                                        [&](const header_entry& entry) { return header_name_equal(entry.name, name); }),
                         state_->headers.end());
}

header_iterator response::find(field name) const {
   return find(field_name(name));
}

header_iterator response::find(std::string_view name) const {
   const auto found = find_header(state_->headers, name);
   if (found == state_->headers.end()) {
      return end();
   }
   return header_iterator{*found};
}

header_iterator response::end() const noexcept {
   return header_iterator{};
}

std::string response::operator[](field name) const {
   return (*this)[field_name(name)];
}

std::string response::operator[](std::string_view name) const {
   if (const auto value = header(name)) {
      return std::string{*value};
   }
   return {};
}

std::optional<std::string_view> response::header(std::string_view name) const {
   const auto found = find_header(state_->headers, name);
   if (found == state_->headers.end()) {
      return std::nullopt;
   }
   return std::string_view{found->text};
}

std::vector<header_entry> response::headers() const {
   return state_->headers;
}

void response::prepare_payload() {
   if (!response_status_allows_payload(state_->status_value)) {
      erase(field::content_length);
      erase(field::transfer_encoding);
      return;
   }
   set(field::content_length, std::to_string(state_->body_value.size()));
}

endpoint_request::endpoint_request()
    : state_(endpoint_state_access::make(forge::net::http::request{}, forge::net::http::response{})) {}

endpoint_request::endpoint_request(const endpoint_request&) noexcept = default;
endpoint_request::endpoint_request(endpoint_request&&) noexcept = default;
endpoint_request& endpoint_request::operator=(const endpoint_request&) noexcept = default;
endpoint_request& endpoint_request::operator=(endpoint_request&&) noexcept = default;
endpoint_request::~endpoint_request() = default;

const forge::net::http::request& endpoint_request::request() const noexcept {
   return state_->request_value;
}

forge::net::http::response& endpoint_request::response() noexcept {
   return state_->response_value;
}

const forge::net::http::response& endpoint_request::response() const noexcept {
   return state_->response_value;
}

std::shared_ptr<endpoint_state> endpoint_state_access::make(forge::net::http::request request_value,
                                                            forge::net::http::response response_value) {
   return std::make_shared<endpoint_state>(endpoint_state{
       .request_value = std::move(request_value),
       .response_value = std::move(response_value),
   });
}

void endpoint_state_access::attach(endpoint_request& target, std::shared_ptr<endpoint_state> state) {
   target.state_ = std::move(state);
}

forge::net::http::response& endpoint_state_access::response(std::shared_ptr<endpoint_state>& state) noexcept {
   return state->response_value;
}

const forge::net::http::response&
endpoint_state_access::response(const std::shared_ptr<endpoint_state>& state) noexcept {
   return state->response_value;
}

response make_text_response(const request& source, status result, std::string body, std::string content_type) {
   auto reply = response{result, source.version()};
   reply.set(field::content_type, content_type);
   reply.body() = std::move(body);
   reply.prepare_payload();
   reply.keep_alive(source.keep_alive());
   return reply;
}

} // namespace forge::net::http
