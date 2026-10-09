module;

#include <cstdint>
#include <algorithm>
#include <coroutine>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

module forge.net.http.router;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.asio.compute;
import forge.net.http.exceptions;
import forge.net.http.assets;
import forge.net.http.middleware;
import forge.net.http.stream;
import forge.net.http.target;
import forge.codec.json;

#include "details/router_match.hxx"

namespace forge::net::http {

using detail::find_path_match;
using detail::match_path;
using detail::parameterized;
using detail::path_exists;

namespace {

std::string http_error_name(int code) {
   switch (code) {
   case 400:
      return "bad_request";
   case 401:
      return "unauthorized";
   case 403:
      return "forbidden";
   case 404:
      return "not_found";
   case 405:
      return "method_not_allowed";
   case 406:
      return "not_acceptable";
   case 409:
      return "conflict";
   case 413:
      return "payload_too_large";
   case 415:
      return "unsupported_media_type";
   case 429:
      return "too_many_requests";
   case 431:
      return "request_header_fields_too_large";
   case 503:
      return "unavailable";
   case 504:
      return "gateway_timeout";
   default:
      return "internal";
   }
}

forge::api::core::status semantic_status_for_http(int code) noexcept {
   using forge::api::core::status;
   switch (code) {
   case 401:
      return status::unauthenticated;
   case 403:
      return status::permission_denied;
   case 404:
      return status::not_found;
   case 409:
      return status::conflict;
   case 413:
   case 429:
   case 431:
      return status::resource_exhausted;
   case 503:
      return status::unavailable;
   case 504:
      return status::deadline_exceeded;
   default:
      return code >= 400 && code < 500 ? status::invalid_argument : status::internal;
   }
}

status http_status_for(const forge::exceptions::base& error) {
   if (std::string_view{error.code().category().name()} == "forge.net.http") {
      const auto value = error.code().value();
      if (value >= 400 && value <= 599) {
         return static_cast<status>(value);
      }
   }
   return status::internal_server_error;
}

forge::api::core::error_payload http_error_payload(const forge::exceptions::base& error) {
   if (std::string_view{error.code().category().name()} == "forge.net.http") {
      return forge::api::core::error_payload{
          .error = http_error_name(error.code().value()),
          .message = error.message().empty() ? http_error_name(error.code().value()) : error.message(),
          .retryable = error.code().value() == 429 || error.code().value() == 503 || error.code().value() == 504,
          .status_code = semantic_status_for_http(error.code().value()),
          .identity =
              {
                  .category = error.code().category().name(),
                  .code = static_cast<std::uint32_t>(error.code().value()),
              },
      };
   }

   return forge::api::core::error_payload{
       .error = "internal",
       .message = "internal error",
       .retryable = false,
       .status_code = forge::api::core::status::internal,
       .identity =
           {
               .category = "forge.net.http",
               .code = static_cast<std::uint32_t>(status::internal_server_error),
           },
   };
}

struct stream_transfer_framing {
   std::optional<std::string> content_length;
   std::optional<std::string> transfer_encoding;
};

std::optional<std::string> header_value(const response& value, field name) {
   if (const auto found = value.find(name); found != value.end()) {
      return std::string{found->value()};
   }
   return std::nullopt;
}

stream_transfer_framing capture_stream_transfer_framing(const response& value) {
   return stream_transfer_framing{
       .content_length = header_value(value, field::content_length),
       .transfer_encoding = header_value(value, field::transfer_encoding),
   };
}

void restore_stream_transfer_framing(response& value, const stream_transfer_framing& framing) {
   value.erase(field::content_length);
   value.erase(field::transfer_encoding);
   if (framing.content_length.has_value()) {
      value.set(field::content_length, *framing.content_length);
   }
   if (framing.transfer_encoding.has_value()) {
      value.set(field::transfer_encoding, *framing.transfer_encoding);
   }
}

std::string encode_error_payload(const forge::api::core::error_payload& payload) {
   auto encoded = forge::codec::json::write(payload);
   if (encoded.ok()) {
      return std::move(encoded.text);
   }
   return forge::codec::json::write(forge::api::core::make_internal_error_payload()).text;
}

response make_exception_response(const request& request, const forge::exceptions::base& error) {
   return make_text_response(request, http_status_for(error), encode_error_payload(http_error_payload(error)),
                             "application/json");
}

std::vector<std::string> split_route_path(const std::string& path) {
   if (path.empty() || path.front() != '/') {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "route path must start with /");
   }
   if (path == "/") {
      return {};
   }

   auto segments = std::vector<std::string>{};
   auto start = std::size_t{1};
   while (start <= path.size()) {
      const auto separator = path.find('/', start);
      const auto end = separator == std::string::npos ? path.size() : separator;
      segments.push_back(path.substr(start, end - start));
      if (separator == std::string::npos) {
         break;
      }
      start = separator + 1U;
   }
   return segments;
}

template <typename Entry>
bool method_path_exists(const std::vector<Entry>& entries, method verb, const target& parsed_target) {
   for (const auto& entry : entries) {
      if (entry.verb == verb && match_path(entry, parsed_target, nullptr)) {
         return true;
      }
   }
   return false;
}

bool path_prefix_matches(const std::vector<std::string>& prefix, bool trailing_slash, const target& parsed_target) {
   if (prefix.empty()) {
      return true;
   }
   if (parsed_target.segments.size() < prefix.size() ||
       !std::equal(prefix.begin(), prefix.end(), parsed_target.segments.begin())) {
      return false;
   }
   return !trailing_slash || parsed_target.segments.size() > prefix.size() || parsed_target.path.ends_with('/');
}

template <typename Entry>
middleware_list matching_middlewares(const std::vector<Entry>& middlewares, const target& parsed_target) {
   auto result = middleware_list{};
   for (const auto& entry : middlewares) {
      if (path_prefix_matches(entry.path_segments, entry.trailing_slash, parsed_target)) {
         result.push_back(entry.descriptor.handler);
      }
   }
   return result;
}

[[nodiscard]] bool path_prefix_matches(std::string_view path, std::string_view prefix) noexcept {
   return prefix == "/" || path == prefix ||
          (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/');
}

[[nodiscard]] bool reserved_path_matches(const std::vector<std::string>& prefixes,
                                         const target& parsed_target) noexcept {
   return std::ranges::any_of(prefixes,
                              [&](const auto& prefix) { return path_prefix_matches(parsed_target.path, prefix); });
}

[[nodiscard]] bool asset_mounts_overlap(std::string_view left, std::string_view right) noexcept {
   return path_prefix_matches(left, right) || path_prefix_matches(right, left);
}

} // namespace

void router::use(middleware handler) {
   use(middleware_descriptor{
       .id = "__anonymous_" + std::to_string(++anonymous_middleware_id_),
       .phase = middleware_phase::before_handler,
       .order = static_cast<int>(anonymous_middleware_id_),
       .path_prefix = "/",
       .handler = std::move(handler),
   });
}

void router::use(middleware_descriptor descriptor) {
   if (!descriptor.handler) {
      throw exceptions::bad_request{"HTTP middleware handler must not be empty"};
   }
   if (descriptor.id.empty()) {
      descriptor.id = "__anonymous_" + std::to_string(++anonymous_middleware_id_);
   }
   if (descriptor.path_prefix.empty()) {
      descriptor.path_prefix = "/";
   }
   const auto prefix = parse_target(descriptor.path_prefix);
   if (!prefix.query.empty()) {
      throw exceptions::bad_request{"HTTP middleware path prefix must not contain a query"};
   }
   auto prefix_segments = prefix.segments;
   const auto trailing_slash = prefix.path.size() > 1U && prefix.path.ends_with('/');
   if (trailing_slash && !prefix_segments.empty() && prefix_segments.back().empty()) {
      prefix_segments.pop_back();
   }
   for (const auto& existing : middlewares_) {
      if (existing.descriptor.id == descriptor.id) {
         throw exceptions::conflict{"duplicate HTTP middleware id"};
      }
   }
   middlewares_.push_back(middleware_entry{
       .descriptor = std::move(descriptor),
       .path_segments = std::move(prefix_segments),
       .trailing_slash = trailing_slash,
   });
   std::sort(middlewares_.begin(), middlewares_.end(), [](const auto& left, const auto& right) {
      if (left.descriptor.phase != right.descriptor.phase) {
         return static_cast<int>(left.descriptor.phase) < static_cast<int>(right.descriptor.phase);
      }
      if (left.descriptor.order != right.descriptor.order) {
         return left.descriptor.order < right.descriptor.order;
      }
      return left.descriptor.id < right.descriptor.id;
   });
}

void router::get(std::string path, route_handler handler) {
   add_route(method::get, std::move(path), std::move(handler));
}

void router::head(std::string path, route_handler handler) {
   add_route(method::head, std::move(path), std::move(handler));
}

void router::post(std::string path, route_handler handler) {
   add_route(method::post, std::move(path), std::move(handler));
}

void router::put(std::string path, route_handler handler) {
   add_route(method::put, std::move(path), std::move(handler));
}

void router::patch(std::string path, route_handler handler) {
   add_route(method::patch, std::move(path), std::move(handler));
}

void router::del(std::string path, route_handler handler) {
   add_route(method::delete_, std::move(path), std::move(handler));
}

void router::get_stream(std::string path, stream_route_handler handler) {
   add_stream_route(method::get, std::move(path), std::move(handler));
}

void router::head_stream(std::string path, stream_route_handler handler) {
   add_stream_route(method::head, std::move(path), std::move(handler));
}

void router::post_stream(std::string path, stream_route_handler handler) {
   add_stream_route(method::post, std::move(path), std::move(handler));
}

void router::put_stream(std::string path, stream_route_handler handler) {
   add_stream_route(method::put, std::move(path), std::move(handler));
}

void router::patch_stream(std::string path, stream_route_handler handler) {
   add_stream_route(method::patch, std::move(path), std::move(handler));
}

void router::del_stream(std::string path, stream_route_handler handler) {
   add_stream_route(method::delete_, std::move(path), std::move(handler));
}

void router::websocket(std::string path, websocket_route_handler handler) {
   if (std::ranges::any_of(websocket_guarded_routes_, [&](const auto& route) { return route.path == path; }) ||
       std::ranges::any_of(websocket_routes_, [&](const auto& route) { return route.path == path; })) {
      throw exceptions::conflict{"duplicate WebSocket route"};
   }
   auto segments = split_route_path(path);
   websocket_routes_.push_back(websocket_route_entry{
       .path = std::move(path),
       .segments = segments,
       .parameterized = parameterized(segments),
       .handler = std::move(handler),
   });
}

void router::websocket_guarded(std::string path, websocket_upgrade_handler handler) {
   if (!handler || std::ranges::any_of(websocket_guarded_routes_, [&](const auto& route) { return route.path == path; }) ||
       std::ranges::any_of(websocket_routes_, [&](const auto& route) { return route.path == path; })) {
      throw exceptions::conflict{"invalid or duplicate WebSocket route"};
   }
   auto segments = split_route_path(path);
   websocket_guarded_routes_.push_back(websocket_guarded_route_entry{
       .path = std::move(path), .segments = segments, .parameterized = parameterized(segments),
       .handler = std::move(handler),
   });
}

void router::mount_assets(asset_mount value, forge::asio::compute::executor read_executor) {
   mount_assets(asset_bundle{std::move(value), std::move(read_executor)});
}

void router::mount_assets(asset_bundle bundle) {
   if (std::ranges::any_of(reserved_path_prefixes_, [&](const auto& prefix) {
          return asset_mounts_overlap(prefix, bundle.path()) && (bundle.path() != "/" || prefix == "/");
       })) {
      throw exceptions::conflict{"HTTP asset mount overlaps a reserved API path prefix"};
   }
   for (const auto& existing : asset_mounts_) {
      if (asset_mounts_overlap(existing.path(), bundle.path())) {
         throw exceptions::conflict{"HTTP asset mounts must not overlap"};
      }
   }
   asset_mounts_.push_back(std::move(bundle));
}

void router::reserve_path_prefix(std::string path) {
   const auto parsed = parse_target(path);
   if (!parsed.query.empty()) {
      throw exceptions::bad_request{"reserved HTTP path prefix must not contain a query"};
   }
   auto prefix = parsed.path;
   while (prefix.size() > 1U && prefix.ends_with('/')) {
      prefix.pop_back();
   }
   if (std::ranges::any_of(asset_mounts_, [&](const auto& mount) {
          return asset_mounts_overlap(prefix, mount.path()) && (mount.path() != "/" || prefix == "/");
       })) {
      throw exceptions::conflict{"reserved HTTP path prefix overlaps an asset mount"};
   }
   if (!std::ranges::contains(reserved_path_prefixes_, prefix)) {
      reserved_path_prefixes_.push_back(std::move(prefix));
   }
}

boost::asio::awaitable<response> router::handle(route_context& context) const {
   try {
      auto params = std::unordered_map<std::string, std::string>{};
      for (const auto prefer_parameterized : {false, true}) {
         for (const auto& route : routes_) {
            if (route.verb != context.request.method() || route.parameterized != prefer_parameterized) {
               continue;
            }
            if (!match_path(route, context.parsed_target, &params)) {
               continue;
            }

            context.route_params = std::move(params);
            co_return co_await run_middleware_chain(matching_middlewares(middlewares_, context.parsed_target), context,
                                                    route.handler);
         }
      }

      if (path_exists(routes_, context.parsed_target)) {
         co_return make_text_response(context.request, status::method_not_allowed, "method not allowed");
      }
      if (path_exists(stream_routes_, context.parsed_target)) {
         co_return make_text_response(context.request, status::method_not_allowed, "method not allowed");
      }
      if (path_exists(websocket_routes_, context.parsed_target)) {
         co_return make_text_response(context.request, status::upgrade_required, "websocket upgrade required");
      }
      if (reserved_path_matches(reserved_path_prefixes_, context.parsed_target)) {
         co_return make_text_response(context.request, status::not_found, "not found");
      }
      if (std::ranges::any_of(asset_mounts_,
                              [&](const auto& mount) { return mount.contains(context.parsed_target.path); })) {
         co_return make_text_response(context.request, status::method_not_allowed, "method not allowed");
      }
      co_return make_text_response(context.request, status::not_found, "not found");
   } catch (const forge::exceptions::base& error) {
      co_return make_exception_response(context.request, error);
   } catch (const std::exception&) {
      co_return make_text_response(context.request, status::internal_server_error,
                                   encode_error_payload(forge::api::core::error_payload{
                                       .error = "internal",
                                       .message = "internal error",
                                       .identity =
                                           {
                                               .category = "forge.net.http",
                                               .code = static_cast<std::uint32_t>(status::internal_server_error),
                                           },
                                   }),
                                   "application/json");
   } catch (...) {
      co_return make_text_response(context.request, status::internal_server_error, "internal server error");
   }
}

bool router::can_handle_stream(route_context& context) const {
   if (method_path_exists(routes_, context.request.method(), context.parsed_target)) {
      return false;
   }
   if (method_path_exists(stream_routes_, context.request.method(), context.parsed_target)) {
      return true;
   }
   if (path_exists(routes_, context.parsed_target) || path_exists(stream_routes_, context.parsed_target) ||
       path_exists(websocket_routes_, context.parsed_target) ||
       reserved_path_matches(reserved_path_prefixes_, context.parsed_target)) {
      return false;
   }
   return std::ranges::any_of(asset_mounts_, [&](const auto& mount) {
      return mount.serves(context.request.method(), context.parsed_target.path);
   });
}

boost::asio::awaitable<stream_response> router::handle_stream(stream_request& request) const {
   auto& context = request.context;
   try {
      auto run_stream = [this, &context](auto&& handler) -> boost::asio::awaitable<stream_response> {
         auto result = std::optional<stream_response>{};
         auto framing = stream_transfer_framing{};
         auto stream_state = stream_pass_through_state{};
         auto head =
             co_await run_middleware_chain(matching_middlewares(middlewares_, context.parsed_target), context,
                                           [handler = std::forward<decltype(handler)>(handler), &result, &framing,
                                            &stream_state](route_context&) mutable -> boost::asio::awaitable<response> {
                                              result = co_await handler();
                                              framing = capture_stream_transfer_framing(result->head);
                                              stream_state = mark_stream_pass_through(result->head);
                                              co_return std::move(result->head);
                                           });
         if (!result.has_value()) {
            clear_stream_pass_through(head);
            co_return stream_response::buffered(std::move(head));
         }
         const auto preserve_stream_body =
             static_cast<bool>(result->body) && is_stream_pass_through(head, stream_state) && head.body().empty();
         clear_stream_pass_through(head);
         if (!preserve_stream_body) {
            co_return stream_response::buffered(std::move(head));
         }
         if (result->body) {
            restore_stream_transfer_framing(head, framing);
         }
         result->head = std::move(head);
         co_return std::move(*result);
      };
      auto params = std::unordered_map<std::string, std::string>{};
      for (const auto prefer_parameterized : {false, true}) {
         for (const auto& route : stream_routes_) {
            if (route.verb != context.request.method() || route.parameterized != prefer_parameterized) {
               continue;
            }
            if (!match_path(route, context.parsed_target, &params)) {
               continue;
            }

            context.route_params = std::move(params);
            co_return co_await run_stream([&route, &request]() -> boost::asio::awaitable<stream_response> {
               co_return co_await route.handler(request);
            });
         }
      }

      if (path_exists(routes_, context.parsed_target) || path_exists(websocket_routes_, context.parsed_target) ||
          reserved_path_matches(reserved_path_prefixes_, context.parsed_target)) {
         co_return stream_response::buffered(make_text_response(context.request, status::not_found, "not found"));
      }
      if (path_exists(stream_routes_, context.parsed_target)) {
         co_return stream_response::buffered(
             make_text_response(context.request, status::method_not_allowed, "method not allowed"));
      }
      for (const auto& mount : asset_mounts_) {
         if (mount.serves(context.request.method(), context.parsed_target.path)) {
            co_return co_await run_stream([&mount, &request]() -> boost::asio::awaitable<stream_response> {
               co_return co_await mount.serve(request);
            });
         }
      }
      if (std::ranges::any_of(asset_mounts_,
                              [&](const auto& mount) { return mount.contains(context.parsed_target.path); })) {
         co_return stream_response::buffered(
             make_text_response(context.request, status::method_not_allowed, "method not allowed"));
      }
      co_return stream_response::buffered(make_text_response(context.request, status::not_found, "not found"));
   } catch (const forge::exceptions::base& error) {
      co_return stream_response::buffered(make_exception_response(context.request, error));
   } catch (const std::exception&) {
      co_return stream_response::buffered(
          make_text_response(context.request, status::internal_server_error,
                             encode_error_payload(forge::api::core::error_payload{
                                 .error = "internal",
                                 .message = "internal error",
                                 .identity =
                                     {
                                         .category = "forge.net.http",
                                         .code = static_cast<std::uint32_t>(status::internal_server_error),
                                     },
                             }),
                             "application/json"));
   } catch (...) {
      co_return stream_response::buffered(
          make_text_response(context.request, status::internal_server_error, "internal server error"));
   }
}

std::optional<websocket_route_handler> router::match_websocket(route_context& context) const {
   if (context.request.method() != method::get) {
      return std::nullopt;
   }

   auto params = std::unordered_map<std::string, std::string>{};
   if (const auto* route = find_path_match(websocket_routes_, context.parsed_target, params); route != nullptr) {
      context.route_params = std::move(params);
      return route->handler;
   }
   return std::nullopt;
}

std::optional<websocket_upgrade_handler> router::match_websocket_guarded(route_context& context) const {
   if (context.request.method() != method::get) {
      return std::nullopt;
   }
   auto params = std::unordered_map<std::string, std::string>{};
   if (const auto* route = find_path_match(websocket_guarded_routes_, context.parsed_target, params); route != nullptr) {
      context.route_params = std::move(params);
      return route->handler;
   }
   return std::nullopt;
}

void router::add_route(method verb, std::string path, route_handler handler) {
   auto segments = split_route_path(path);
   for (const auto& route : routes_) {
      if (route.verb == verb && route.path == path) {
         throw exceptions::conflict{"duplicate HTTP route"};
      }
   }
   for (const auto& route : stream_routes_) {
      if (route.verb == verb && route.path == path) {
         throw exceptions::conflict{"duplicate HTTP route"};
      }
   }
   routes_.push_back(route_entry{
       .verb = verb,
       .path = std::move(path),
       .segments = segments,
       .parameterized = parameterized(segments),
       .handler = std::move(handler),
   });
}

void router::add_stream_route(method verb, std::string path, stream_route_handler handler) {
   auto segments = split_route_path(path);
   for (const auto& route : routes_) {
      if (route.verb == verb && route.path == path) {
         throw exceptions::conflict{"duplicate HTTP stream route"};
      }
   }
   for (const auto& route : stream_routes_) {
      if (route.verb == verb && route.path == path) {
         throw exceptions::conflict{"duplicate HTTP stream route"};
      }
   }
   stream_routes_.push_back(stream_route_entry{
       .verb = verb,
       .path = std::move(path),
       .segments = segments,
       .parameterized = parameterized(segments),
       .handler = std::move(handler),
   });
}

} // namespace forge::net::http
