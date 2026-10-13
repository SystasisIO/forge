#include "common.hxx"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import forge.auth.oauth2.token_provider;
import forge.auth.oauth2.interactive_authorizer;

#include "workflow.hxx"

namespace auth_live {
namespace {

[[noreturn]] void invalid_config() {
   throw std::runtime_error{"invalid fixture configuration"};
}

bool safe_text(std::string_view text, std::size_t maximum = 512) {
   return !text.empty() && text.size() <= maximum &&
          std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}

bool uuid(std::string_view text) {
   if (text.size() != 36) {
      return false;
   }
   for (std::size_t i = 0; i < text.size(); ++i) {
      if (i == 8 || i == 13 || i == 18 || i == 23) {
         if (text[i] != '-') {
            return false;
         }
      } else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
         return false;
      }
   }
   return true;
}

int number(const std::string& value, int minimum, int maximum) {
   int result = 0;
   const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
   if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
       result < minimum || result > maximum) {
      invalid_config();
   }
   return result;
}

std::string read_config(const std::string& path) {
   struct descriptor {
      int value;
      ~descriptor() {
         if (value >= 0) {
            ::close(value);
         }
      }
   } file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)};
   struct stat metadata{};
   if (file.value < 0 || ::fstat(file.value, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
       metadata.st_size < 0 || metadata.st_size > 8192) {
      invalid_config();
   }
   std::string data;
   std::array<char, 8193> buffer{};
   while (data.size() <= 8192) {
      const auto count = ::read(file.value, buffer.data(), buffer.size() - data.size());
      if (count < 0) {
         if (errno == EINTR) {
            continue;
         }
         invalid_config();
      }
      if (count == 0) {
         break;
      }
      data.append(buffer.data(), static_cast<std::size_t>(count));
   }
   if (data.size() > 8192 || !std::all_of(data.begin(), data.end(), [](unsigned char c) {
          return c == '\n' || (c >= 32 && c <= 126);
       })) {
      invalid_config();
   }
   return data;
}

void set(options& config, const std::string& key, const std::string& value) {
   if (key == "issuer") {
      config.issuer = value;
   } else if (key == "client") {
      config.client = value;
   } else if (key == "account") {
      config.account = value;
   } else if (key == "run-id") {
      config.run_id = value;
   } else if (key == "flow") {
      config.flow = value;
   } else if (key == "audience") {
      config.audience = value;
   } else if (key == "minimum-validity") {
      config.minimum_validity = std::chrono::seconds{number(value, 0, 60)};
   } else if (key == "timeout") {
      config.timeout = std::chrono::seconds{number(value, 5, 900)};
   } else if (key == "scopes") {
      config.scopes.clear();
      std::size_t start = 0;
      while (start <= value.size()) {
         const auto end = value.find(',', start);
         const auto scope = value.substr(start, end == std::string::npos ? end : end - start);
         if (!safe_text(scope, 64) || config.scopes.size() == 8 ||
             std::find(config.scopes.begin(), config.scopes.end(), scope) != config.scopes.end()) {
            invalid_config();
         }
         config.scopes.push_back(scope);
         if (end == std::string::npos) {
            break;
         }
         start = end + 1;
      }
   } else {
      // Unknown keys (including token/password/client-secret) are never accepted or echoed.
      invalid_config();
   }
}

options parse(int argc, char** argv) {
   options config;
   std::map<std::string, std::string> cli;
   if (argc > 25 || argc % 2 == 0) {
      invalid_config();
   }
   for (int i = 1; i < argc; i += 2) {
      const auto argument = std::string_view{argv[i]};
      const auto value = std::string_view{argv[i + 1]};
      if (!argument.starts_with("--") || !safe_text(argument, 32) || !safe_text(value, 1024) ||
          !cli.emplace(std::string{argument.substr(2)}, std::string{value}).second) {
         invalid_config();
      }
   }
   if (const auto file = cli.find("config"); file != cli.end()) {
      const auto data = read_config(file->second);
      std::map<std::string, bool> seen;
      std::size_t start = 0;
      while (start < data.size()) {
         const auto end = data.find('\n', start);
         const auto line = data.substr(start, end == std::string::npos ? end : end - start);
         if (!line.empty() && line.front() != '#') {
            const auto equals = line.find('=');
            if (equals == std::string::npos) {
               invalid_config();
            }
            const auto key = line.substr(0, equals);
            const auto value = line.substr(equals + 1);
            if (!safe_text(key, 32) || !safe_text(value) || !seen.emplace(key, true).second) {
               invalid_config();
            }
            set(config, key, value);
         }
         if (end == std::string::npos) {
            break;
         }
         start = end + 1;
      }
      cli.erase(file);
   }
   for (const auto& [key, value] : cli) {
      set(config, key, value);
   }
   if (!safe_text(config.issuer) || !config.issuer.starts_with("https://") || config.issuer.size() <= 8 ||
       config.issuer.find_first_of("@?#\\") != std::string::npos || config.issuer[8] == '/' ||
       !safe_text(config.client, 128) || !uuid(config.run_id) || config.account != "live-" + config.run_id ||
       (config.audience && !safe_text(*config.audience)) ||
       (config.flow != "authorize" && config.flow != "acquire" && config.flow != "resume" &&
        config.flow != "refresh" && config.flow != "invalidate")) {
      invalid_config();
   }
   return config;
}

int failure(std::exception_ptr error) {
   try {
      std::rethrow_exception(error);
   } catch (const forge::auth::oauth2::exceptions::setup_required&) {
      event("BLOCKED", "setup_required");
      return 3;
   } catch (const forge::auth::oauth2::exceptions::unsupported_request&) {
      event("FAIL", "unsupported_request");
      return 2;
   } catch (const forge::auth::oauth2::exceptions::invalid_request&) {
      event("FAIL", "invalid_request");
      return 2;
   } catch (const forge::auth::oauth2::exceptions::canceled&) {
      event("BLOCKED", "canceled");
      return 3;
   } catch (...) {
      event("FAIL", "provider_or_assertion_failure");
      return 1;
   }
}

forge::auth::oauth2::token_request request(const options& config) {
   return {.scopes = config.scopes, .audience = config.audience, .minimum_validity = config.minimum_validity};
}

std::chrono::system_clock::time_point inspect(const forge::auth::oauth2::access_token& token,
                                            const options& config) {
   const auto& metadata = token.metadata();
   const auto now = std::chrono::system_clock::now();
   if (metadata.issuer != config.issuer || !metadata.expires_at || *metadata.expires_at <= now ||
       *metadata.expires_at + std::chrono::seconds{1} < now + config.minimum_validity) {
      throw std::runtime_error{"public token metadata did not satisfy fixture constraints"};
   }
   return *metadata.expires_at;
}

long long epoch(std::chrono::system_clock::time_point expiry) {
   return std::chrono::duration_cast<std::chrono::seconds>(expiry.time_since_epoch()).count();
}

} // namespace

void event(std::string_view status, std::string_view name, bool success, long long expires_at,
           unsigned issuer_match_count) {
   std::cout << "{\"status\":\"" << status << "\",\"event\":\"" << name
             << "\",\"success\":" << (success ? "true" : "false")
             << ",\"expires_at\":" << expires_at << ",\"issuer_match_count\":" << issuer_match_count
             << "}" << std::endl;
}

boost::asio::awaitable<void> probe(forge::auth::oauth2::token_provider& provider,
                                 forge::auth::oauth2::interactive_authorizer* authorizer,
                                 const options& config) {
   if (config.flow == "authorize" || config.flow == "invalidate") {
      if (authorizer == nullptr) {
         throw forge::auth::oauth2::exceptions::unsupported_request{"agent lifecycle is external"};
      }
      if (config.flow == "invalidate") {
         co_await authorizer->async_invalidate();
      }
      bool absent = false;
      try {
         auto token = co_await provider.async_access_token(request(config));
      } catch (const forge::auth::oauth2::exceptions::setup_required&) {
         absent = true;
      }
      if (!absent) {
         throw std::runtime_error{"fixture state must be absent"};
      }
      if (config.flow == "invalidate") {
         event("EVENT", "local_state_absent", true);
         co_return;
      }
      event("READY", "browser_user_action_required");
      co_await authorizer->async_authorize(request(config));
      event("EVENT", "authorization_completed", true);
   }

   std::chrono::system_clock::time_point previous;
   {
      auto token = co_await provider.async_access_token(request(config));
      previous = inspect(token, config);
      event("EVENT", "token_observed", true, epoch(previous), 1);
   }
   if (config.flow != "refresh") {
      co_return;
   }

   const auto delay = previous + std::chrono::seconds{2} - std::chrono::system_clock::now();
   if (delay <= std::chrono::system_clock::duration::zero() || delay > std::chrono::seconds{300}) {
      throw forge::auth::oauth2::exceptions::unsupported_request{"expiry wait exceeds fixture bound"};
   }
   event("EVENT", "waiting_for_actual_expiry", false, epoch(previous), 1);
   boost::asio::steady_timer wait{co_await boost::asio::this_coro::executor};
   wait.expires_after(std::chrono::duration_cast<boost::asio::steady_timer::duration>(delay));
   co_await wait.async_wait(boost::asio::use_awaitable);
   if (std::chrono::system_clock::now() <= previous) {
      throw std::runtime_error{"clock did not pass the original expiry"};
   }
   auto renewed = co_await provider.async_access_token(request(config));
   const auto expiry = inspect(renewed, config);
   if (expiry <= previous) {
      throw std::runtime_error{"expiry did not advance"};
   }
   event("EVENT", "renewed_after_expiry", true, epoch(expiry), 1);
}

int run(int argc, char** argv, job operation, std::function<void()> run_ui, std::function<void()> stop_ui) {
   options config;
   try {
      config = parse(argc, argv);
   } catch (...) {
      event("FAIL", "configuration_invalid");
      return 2;
   }

   boost::asio::io_context io;
   boost::asio::steady_timer deadline{io};
   boost::asio::cancellation_signal cancellation;
   bool timed_out = false;
   int result = 1;
   deadline.expires_after(config.timeout);
   deadline.async_wait([&](const boost::system::error_code& error) {
      if (error) {
         return;
      }
      timed_out = true;
      event("BLOCKED", "deadline");
      cancellation.emit(boost::asio::cancellation_type::all);
      // liboidc-agent has no hard cancel. This disposable process is the hard boundary.
      deadline.expires_after(std::chrono::seconds{5});
      deadline.async_wait([](const boost::system::error_code& grace_error) {
         if (!grace_error) {
            std::_Exit(124);
         }
      });
   });
   boost::asio::co_spawn(io, operation(config), boost::asio::bind_cancellation_slot(cancellation.slot(),
      [&](std::exception_ptr error) {
         result = timed_out ? 124 : (error ? failure(error) : 0);
         deadline.cancel();
         if (result == 0) {
            event("PASS", config.flow, true);
         }
         if (stop_ui) {
            stop_ui();
         }
      }));
   if (run_ui) {
      std::jthread worker{[&] { io.run(); }};
      run_ui();
      worker.join();
   } else {
      io.run();
   }
   return result;
}

} // namespace auth_live
