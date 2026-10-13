module;

#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module forge.net.http.types;

export namespace forge::net::http {

enum class field {
   accept,
   accept_ranges,
   authorization,
   connection,
   content_length,
   content_range,
   content_type,
   cookie,
   etag,
   expect,
   host,
   if_modified_since,
   if_none_match,
   if_range,
   last_modified,
   range,
   retry_after,
   server,
   transfer_encoding,
   user_agent,
};

enum class method {
   unknown,
   delete_,
   get,
   head,
   options,
   patch,
   post,
   put,
};

[[nodiscard]] bool is_idempotent(method value) noexcept;

enum class status : unsigned {
   continue_ = 100,
   ok = 200,
   created = 201,
   accepted = 202,
   no_content = 204,
   partial_content = 206,
   found = 302,
   not_modified = 304,
   bad_request = 400,
   unauthorized = 401,
   forbidden = 403,
   not_found = 404,
   method_not_allowed = 405,
   not_acceptable = 406,
   conflict = 409,
   payload_too_large = 413,
   unsupported_media_type = 415,
   range_not_satisfiable = 416,
   upgrade_required = 426,
   too_many_requests = 429,
   request_header_fields_too_large = 431,
   internal_server_error = 500,
   bad_gateway = 502,
   service_unavailable = 503,
   gateway_timeout = 504,
};

struct header_entry {
   std::string name;
   std::string text;

   [[nodiscard]] std::string_view name_string() const noexcept {
      return name;
   }

   [[nodiscard]] std::string_view value() const noexcept {
      return text;
   }
};

[[nodiscard]] bool header_name_equal(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::optional<std::string_view> find_header(std::span<const header_entry> headers,
                                                          std::string_view name) noexcept;
void set_header(std::vector<header_entry>& headers, std::string name, std::string value);

class header_iterator {
 public:
   header_iterator() = default;

   [[nodiscard]] const header_entry& operator*() const noexcept;
   [[nodiscard]] const header_entry* operator->() const noexcept;

   friend bool operator==(const header_iterator& left, const header_iterator& right) noexcept;
   friend bool operator!=(const header_iterator& left, const header_iterator& right) noexcept {
      return !(left == right);
   }

 private:
   explicit header_iterator(std::optional<header_entry> value);

   std::optional<header_entry> value_;

   friend class request;
   friend class response;
};

[[nodiscard]] std::string_view field_name(field value) noexcept;
std::ostream& operator<<(std::ostream& stream, method value);
std::ostream& operator<<(std::ostream& stream, status value);

class request {
 public:
   request();
   request(forge::net::http::method method_value, std::string target_value, unsigned version_value);
   ~request();

   request(const request&);
   request(request&&) noexcept;
   request& operator=(const request&);
   request& operator=(request&&) noexcept;

   [[nodiscard]] forge::net::http::method method() const noexcept;
   void method(forge::net::http::method value) noexcept;

   [[nodiscard]] std::string_view target() const noexcept;
   void target(std::string value);
   void target(std::string_view value);
   void target(const char* value);

   [[nodiscard]] unsigned version() const noexcept;
   void version(unsigned value) noexcept;

   [[nodiscard]] bool keep_alive() const noexcept;
   void keep_alive(bool value) noexcept;

   [[nodiscard]] std::string& body() noexcept;
   [[nodiscard]] const std::string& body() const noexcept;

   void set(field name, std::string_view value);
   void set(std::string_view name, std::string_view value);
   void insert(std::string_view name, std::string_view value);
   void erase(field name);
   void erase(std::string_view name);

   [[nodiscard]] header_iterator find(field name) const;
   [[nodiscard]] header_iterator find(std::string_view name) const;
   [[nodiscard]] header_iterator end() const noexcept;
   [[nodiscard]] std::string operator[](field name) const;
   [[nodiscard]] std::string operator[](std::string_view name) const;
   [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
   [[nodiscard]] std::vector<header_entry> headers() const;

   void prepare_payload();

 private:
   struct state;

   std::shared_ptr<state> state_;
};

class response {
 public:
   response();
   response(status status_value, unsigned version_value);
   ~response();

   response(const response&);
   response(response&&) noexcept;
   response& operator=(const response&);
   response& operator=(response&&) noexcept;

   [[nodiscard]] status result() const noexcept;
   void result(status value) noexcept;
   [[nodiscard]] unsigned result_int() const noexcept;

   [[nodiscard]] unsigned version() const noexcept;
   void version(unsigned value) noexcept;

   [[nodiscard]] bool keep_alive() const noexcept;
   void keep_alive(bool value) noexcept;

   [[nodiscard]] std::string& body() noexcept;
   [[nodiscard]] const std::string& body() const noexcept;

   void set(field name, std::string_view value);
   void set(std::string_view name, std::string_view value);
   void insert(std::string_view name, std::string_view value);
   void erase(field name);
   void erase(std::string_view name);

   [[nodiscard]] header_iterator find(field name) const;
   [[nodiscard]] header_iterator find(std::string_view name) const;
   [[nodiscard]] header_iterator end() const noexcept;
   [[nodiscard]] std::string operator[](field name) const;
   [[nodiscard]] std::string operator[](std::string_view name) const;
   [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
   [[nodiscard]] std::vector<header_entry> headers() const;

   void prepare_payload();

 private:
   struct state;

   std::shared_ptr<state> state_;
};

struct endpoint_state;

class endpoint_request {
 public:
   endpoint_request();
   endpoint_request(const endpoint_request&) noexcept;
   endpoint_request(endpoint_request&&) noexcept;
   endpoint_request& operator=(const endpoint_request&) noexcept;
   endpoint_request& operator=(endpoint_request&&) noexcept;
   virtual ~endpoint_request();

   [[nodiscard]] const forge::net::http::request& request() const noexcept;
   [[nodiscard]] forge::net::http::response& response() noexcept;
   [[nodiscard]] const forge::net::http::response& response() const noexcept;

 private:
   std::shared_ptr<endpoint_state> state_;

   friend struct endpoint_state_access;
};

struct endpoint_state_access {
   [[nodiscard]] static std::shared_ptr<endpoint_state> make(forge::net::http::request request_value,
                                                             forge::net::http::response response_value);
   static void attach(endpoint_request& target, std::shared_ptr<endpoint_state> state);
   [[nodiscard]] static forge::net::http::response& response(std::shared_ptr<endpoint_state>& state) noexcept;
   [[nodiscard]] static const forge::net::http::response&
   response(const std::shared_ptr<endpoint_state>& state) noexcept;
};

[[nodiscard]] response make_text_response(const request& source, status result, std::string body,
                                          std::string content_type = "text/plain");

} // namespace forge::net::http
