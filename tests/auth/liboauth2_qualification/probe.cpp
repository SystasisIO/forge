#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unistd.h>

extern "C" {
#include <oauth2/oauth2.h>
#include <oauth2/mem.h>
#include <oauth2/version.h>
}

namespace {

constexpr auto opaque_token = "forge-f0-fabricated-token";
// A compact JWT with a deliberately invalid HS256 signature; no JWT engine here.
constexpr auto invalid_jwt =
   "eyJ0eXAiOiJKV1QiLA0KICJhbGciOiJIUzI1NiJ9."
   "eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAs"
   "DQogImh0dHA6Ly9leGFtcGxlLmNvbS9pc19yb290Ijp0cnVlfQ."
   "AAAAtJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";

struct observation {
   bool accepted = false;
   oauth2_http_status_code_t status = 0;
   bool completed = false;
};

void discard_log(oauth2_log_sink_t*, const char*, unsigned long,
                 const char*, oauth2_log_level_t, const char*) {}

void hard_deadline(int) {
   constexpr char output[] =
      "{\"case\":\"deadline\",\"accepted\":false,\"status\":0,"
      "\"completed\":false}\n";
   static_cast<void>(::write(STDOUT_FILENO, output, sizeof(output) - 1));
   std::_Exit(3);
}

struct vendor_state {
   oauth2_log_t* log = nullptr;
   oauth2_cfg_token_verify_t* verify = nullptr;
   json_t* payload = nullptr;

   ~vendor_state() {
      if (payload != nullptr)
         json_decref(payload);
      if (verify != nullptr)
         oauth2_cfg_token_verify_free(nullptr, verify);
      if (log != nullptr)
         oauth2_shutdown(log);
   }
};

bool supported_mode(std::string_view mode) {
   return mode == "invalid-jwt" || mode == "jwks-503" ||
      mode == "jwks-unreachable" || mode == "inactive" ||
      mode == "introspection-503" || mode == "introspection-timeout" ||
      mode == "redirect-307" || mode == "redirect-308";
}

bool loopback_endpoint(std::string_view endpoint) {
   constexpr std::string_view prefix = "http://127.0.0.1:";
   if (!endpoint.starts_with(prefix))
      return false;
   endpoint.remove_prefix(prefix.size());
   const auto slash = endpoint.find('/');
   if (slash == std::string_view::npos)
      return false;
   unsigned port = 0;
   const auto end = endpoint.data() + slash;
   const auto parsed = std::from_chars(endpoint.data(), end, port);
   return parsed.ec == std::errc{} && parsed.ptr == end &&
      port > 0 && port <= 65535 &&
      endpoint.substr(slash).find_first_not_of(
         "/abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string_view::npos;
}

observation characterize(std::string_view mode, const char* endpoint) {
   observation result;
   if (std::strcmp(oauth2_version(), "2.4.0") != 0 ||
       std::strcmp(OAUTH2_PACKAGE_VERSION, "2.4.0") != 0)
      return result;

   vendor_state state;
   auto* sink = oauth2_log_sink_create(OAUTH2_LOG_ERROR, discard_log, nullptr);
   if (sink == nullptr)
      return result;
   state.log = oauth2_init(OAUTH2_LOG_ERROR, sink);
   if (state.log == nullptr) {
      oauth2_mem_free(sink);
      oauth2_shutdown(nullptr);
      return result;
   }

   const char* type = "introspect";
   const char* value = endpoint;
   const char* token = opaque_token;
   const char* options =
      "introspect.http_timeout=1&introspect.http_retries=0&"
      "introspect.auth=client_secret_post&"
      "client_id=forge-f0-client&"
      "client_secret=forge-f0-fabricated-client-secret";
   if (mode == "invalid-jwt") {
      type = "plain";
      value = "forge-f0-fabricated-verification-key";
      token = invalid_jwt;
      options = "verify.exp=required";
   } else if (mode.starts_with("jwks-")) {
      type = "jwks_uri";
      token = invalid_jwt;
      options = "jwks_uri.http_timeout=1&jwks_uri.http_retries=0";
   }

   // NULL call logs avoid even formatting vendor messages with token values.
   auto* error = oauth2_cfg_token_verify_add_options(
      nullptr, &state.verify, type, value, options);
   if (error != nullptr) {
      oauth2_mem_free(error);
      return result;
   }
   result.accepted = oauth2_token_verify(
      nullptr, nullptr, state.verify, token, &state.payload, &result.status);
   result.completed = true;
   return result;
}

} // namespace

int main(int argc, char** argv) {
   const bool valid = argc == 3 && supported_mode(argv[1]) &&
      loopback_endpoint(argv[2]);
   const char* mode = valid ? argv[1] : "usage";
   observation result;
   if (valid) {
      if (std::signal(SIGALRM, hard_deadline) == SIG_ERR)
         return 2;
      ::alarm(20);
      result = characterize(mode, argv[2]);
      ::alarm(0);
   }
   std::printf("{\"case\":\"%s\",\"accepted\":%s,\"status\":%u,"
               "\"completed\":%s}\n",
               mode, result.accepted ? "true" : "false",
               static_cast<unsigned>(result.status),
               result.completed ? "true" : "false");
   return result.completed ? 0 : 2;
}
