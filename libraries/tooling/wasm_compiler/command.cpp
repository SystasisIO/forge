module;

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <exception>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <new>
#include <optional>
#include <llvm/Config/llvm-config.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

module forge.tooling.wasm_compiler.command;

import forge.core.version;
import forge.core.git_revision;
import forge.tooling.wasm_compiler.compiler;
import forge.vm.wasm.optimizing.compiler_protocol;
import forge.raw.raw;

#include "details/launch_options.hxx"

namespace forge::tooling::wasm_compiler::command {
namespace {
namespace oc = vm::wasm::optimizing;
namespace protocol = oc::compiler_protocol;

std::uint64_t number(const char* input) {
   if (input == nullptr)
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "missing compiler helper launch argument");
   const auto text = std::string_view{input};
   auto value = std::uint64_t{0};
   const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
   if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "invalid compiler helper launch argument");
   return value;
}

launch_options parse(int argc, const char* const* argv) {
   // Fixed, private numeric launch contract. Request/profile bytes use Raw,
   // never shell quoting, repeated CLI fields or an argv serialization codec.
   if (argc != 10 || argv == nullptr || argv[1] == nullptr || std::string_view{argv[1]} != "--stdio")
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "invalid compiler helper launch mode");
   auto options = launch_options{};
   options.exchange = {number(argv[2]), number(argv[3]), number(argv[4])};
   options.max_request_bytes = number(argv[5]);
   options.max_response_bytes = number(argv[6]);
   options.limits = {number(argv[7]), number(argv[8]), number(argv[9])};
   if (options.max_request_bytes > raw::max_byte_array_size || options.max_response_bytes > raw::max_byte_array_size)
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "helper launch byte ceiling exceeds Raw");
   return options;
}

void limit(int resource, std::uint64_t value) {
   if (value >= static_cast<std::uint64_t>(RLIM_INFINITY) || value > std::numeric_limits<rlim_t>::max())
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "helper resource limit is not finite");
   auto existing = rlimit{};
   if (::getrlimit(resource, &existing) != 0 ||
       (existing.rlim_max != RLIM_INFINITY && value > static_cast<std::uint64_t>(existing.rlim_max)))
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "helper resource limit is unavailable");
   const auto bounds = rlimit{static_cast<rlim_t>(value), static_cast<rlim_t>(value)};
   if (::setrlimit(resource, &bounds) != 0)
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "helper resource limit cannot be applied");
   auto actual = rlimit{};
   if (::getrlimit(resource, &actual) != 0 || actual.rlim_cur != bounds.rlim_cur || actual.rlim_max != bounds.rlim_max)
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "helper resource limit was not retained");
}

void read_exact(int fd, std::span<std::uint8_t> bytes) {
   while (!bytes.empty()) {
      const auto count = ::read(fd, bytes.data(), bytes.size());
      if (count < 0 && errno == EINTR)
         continue;
      if (count <= 0)
         FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "truncated compiler helper input");
      bytes = bytes.subspan(static_cast<std::size_t>(count));
   }
}

void require_eof(int fd) {
   auto byte = std::uint8_t{};
   auto count = ssize_t{};
   do {
      count = ::read(fd, &byte, 1);
   } while (count < 0 && errno == EINTR);
   if (count != 0)
      FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "compiler helper requires exactly one request");
}

void write_exact(int fd, std::span<const std::uint8_t> bytes) {
   while (!bytes.empty()) {
      // The private inherited endpoint is AF_UNIX, not a pipe. Canceling the
      // parent must produce an ordinary EPIPE/exit, not a process-wide SIGPIPE
      // handler/mask mutation or asynchronous termination during LLVM cleanup.
      const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (count < 0 && errno == EINTR)
         continue;
      if (count <= 0)
         FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "compiler helper output is unavailable");
      bytes = bytes.subspan(static_cast<std::size_t>(count));
   }
}

void send(const protocol::exchange_id& exchange, const crypto::digest::sha256& identity, protocol::message_kind kind,
          std::span<const std::uint8_t> body, std::uint64_t max_bytes) {
   if (body.size() > max_bytes)
      FORGE_THROW_EXCEPTION(oc::exceptions::compile_limit, "compiler helper output exceeds response budget");
   auto header = protocol::header{};
   header.kind = kind;
   header.exchange = exchange;
   header.identity = identity;
   header.body_bytes = body.size();
   write_exact(STDOUT_FILENO, protocol::encode(header));
   write_exact(STDOUT_FILENO, body);
}

protocol::reply compile(const oc::compile_request& request, std::uint64_t max_bytes) {
   auto result = protocol::reply{};
   try {
      auto code = compiler{}.compile(request);
      result.image = code.encode();
      // Include the wrapper, not merely artifact bytes, before response publish.
      if (raw::pack_size(result) > max_bytes)
         FORGE_THROW_EXCEPTION(oc::exceptions::compile_limit, "encoded compiler artifact exceeds response budget");
   } catch (const forge::exceptions::base& error) {
      result.image.clear();
      const auto& category = oc::exceptions::forge_exceptions_category(oc::exceptions::code::invalid_module);
      const auto value = error.code().value();
      if (error.code().category() == category && value >= static_cast<int>(oc::exceptions::code::invalid_module) &&
          value <= static_cast<int>(oc::exceptions::code::cache_corruption))
         result.failure = static_cast<oc::exceptions::code>(value);
      else
         result.failure = oc::exceptions::code::compiler_unavailable;
   } catch (const std::bad_alloc&) {
      result.image.clear();
      result.failure = oc::exceptions::code::compile_limit;
   } catch (...) {
      result.image.clear();
      result.failure = oc::exceptions::code::compiler_unavailable;
   }
   return result;
}

} // namespace

int run(int argc, const char* const* argv) {
   try {
      // Even malformed launch input must not create a helper core dump.
      limit(RLIMIT_CORE, 0);
      const auto options = parse(argc, argv);
      limit(RLIMIT_CPU, options.limits.cpu_seconds);
      limit(RLIMIT_AS, options.limits.address_space_bytes);
      limit(RLIMIT_STACK, options.limits.stack_bytes);

      auto header_bytes = std::vector<std::uint8_t>(protocol::header_size());
      read_exact(STDIN_FILENO, header_bytes);
      const auto header = protocol::decode_header(header_bytes);
      const auto preflight = header.kind == protocol::message_kind::preflight;
      if ((!preflight && header.kind != protocol::message_kind::request) || header.exchange != options.exchange ||
          header.body_bytes > options.max_request_bytes)
         FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "incompatible compiler helper request header");
      auto body = std::vector<std::uint8_t>(static_cast<std::size_t>(header.body_bytes));
      read_exact(STDIN_FILENO, body);
      auto request = std::optional<oc::compile_request>{};
      auto target = oc::target_profile{};
      if (preflight) {
         target = protocol::decode_target(body, options.max_request_bytes);
      } else {
         request.emplace(oc::compile_request::decode(body, options.max_request_bytes));
         if (oc::identify(*request) != header.identity)
            FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "compiler request identity mismatch");
         target = request->configuration.target;
      }
      if (target.llvm_version != LLVM_VERSION_STRING)
         FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "compiler helper request identity/build mismatch");

      auto hello = protocol::hello{};
      hello.forge_version = {core::version_major, core::version_minor, core::version_patch};
      hello.build_revision = forge::git_revision_sha;
      hello.build_timestamp = forge::git_revision_unix_timestamp;
      hello.target = std::move(target);
      hello.limits = options.limits;
      // Shared canonical target validation and actual LLVM/build handshake
      // precede compiler::compile/LLVM initialization, not unchecked argv echo.
      const auto hello_bytes = protocol::encode(hello);
      send(options.exchange, {}, protocol::message_kind::hello, hello_bytes, options.max_response_bytes);

      // Do not enter LLVM until the parent has checked the actual build/target
      // hello. The SAME Raw header is the whole acknowledgement, not a codec.
      read_exact(STDIN_FILENO, header_bytes);
      const auto acknowledgement = protocol::decode_header(header_bytes);
      if (acknowledgement.kind != protocol::message_kind::hello_ack || acknowledgement.exchange != options.exchange ||
          acknowledgement.identity != header.identity || acknowledgement.body_bytes != 0)
         FORGE_THROW_EXCEPTION(oc::exceptions::compiler_unavailable, "incompatible compiler hello acknowledgement");
      // Parent closes stdin after the ack; duplicate/trailing input is rejected
      // before compile. Moving this from before hello avoids a handshake deadlock.
      require_eof(STDIN_FILENO);
      // A real resource/build/profile preflight returns only EOF + successful
      // physical process exit. Never invoke the parser, LLVM or a fake module.
      if (preflight)
         return 0;
      const auto result = compile(*request, options.max_response_bytes);
      const auto reply_bytes = protocol::encode(result);
      send(options.exchange, header.identity, protocol::message_kind::reply, reply_bytes, options.max_response_bytes);
      return 0;
   } catch (...) {
      // Parent observes EOF/exit and typed protocol failure; never parses text
      // or accepts a partial handshake as module/compiler success.
      return 2;
   }
}

} // namespace forge::tooling::wasm_compiler::command
