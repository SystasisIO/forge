#include "details/run_worker.hxx"
#include "details/backend.hxx"
#include "details/memory.hxx"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <unistd.h>

import forge.expression.worker_protocol;

namespace forge::expression::detail {
namespace {

namespace protocol = worker_protocol;

constexpr std::size_t program_limit = 64;

enum class read_result { complete, closed, failed };

read_result read_exact(std::span<std::uint8_t> output) noexcept {
   std::size_t received = 0;
   while (received != output.size()) {
      const auto count = ::read(STDIN_FILENO, output.data() + received, output.size() - received);
      if (count == 0) {
         return received == 0 ? read_result::closed : read_result::failed;
      }
      if (count < 0) {
         if (errno == EINTR) {
            continue;
         }
         return read_result::failed;
      }
      received += static_cast<std::size_t>(count);
   }
   return read_result::complete;
}

bool write_exact(std::span<const std::uint8_t> input) noexcept {
   std::size_t sent = 0;
   while (sent != input.size()) {
      const auto count = ::write(STDOUT_FILENO, input.data() + sent, input.size() - sent);
      if (count < 0 && errno == EINTR) {
         continue;
      }
      if (count <= 0) {
         return false;
      }
      sent += static_cast<std::size_t>(count);
   }
   return true;
}

backend::value_view view(const forge::variant& value);

backend::value_view element(const void* context, std::size_t index) {
   return view((*static_cast<const forge::variants*>(context))[index]);
}

backend::value_view mapped(const void* context, std::size_t index) {
   return view((static_cast<const forge::variant_object*>(context)->begin() + index)->value());
}

std::string_view key(const void* context, std::size_t index) {
   return (static_cast<const forge::variant_object*>(context)->begin() + index)->key();
}

// The decoder owns the activation until evaluate() returns. These callbacks
// only borrow its strings and containers; no second owning value tree is built.
backend::value_view view(const forge::variant& value) {
   backend::value_view output;
   switch (value.get_type()) {
   case forge::variant::int64_type:
      output.kind = backend::value_kind::int64;
      output.i64 = value.as_int64();
      break;
   case forge::variant::uint64_type:
      output.kind = backend::value_kind::uint64;
      output.u64 = value.as_uint64();
      break;
   case forge::variant::bool_type:
      output.kind = backend::value_kind::boolean;
      output.boolean = value.as_bool();
      break;
   case forge::variant::string_type:
      output.kind = backend::value_kind::string;
      output.text = value.get_string();
      break;
   case forge::variant::array_type:
      output.kind = backend::value_kind::list;
      output.context = &value.get_array();
      output.count = value.get_array().size();
      output.element = element;
      break;
   case forge::variant::object_type:
      output.kind = backend::value_kind::map;
      output.context = &value.get_object();
      output.count = value.get_object().size();
      output.element = mapped;
      output.key = key;
      break;
   default:
      break;
   }
   return output;
}

protocol::status convert(backend::status value) noexcept {
   switch (value) {
   case backend::status::ok:
      return protocol::status::ok;
   case backend::status::invalid_profile:
      return protocol::status::invalid_profile;
   case backend::status::invalid_program:
      return protocol::status::invalid_program;
   case backend::status::invalid_input:
      return protocol::status::invalid_input;
   case backend::status::resource_exhausted:
      return protocol::status::resource_exhausted;
   case backend::status::evaluation_failed:
      return protocol::status::evaluation_failed;
   default:
      return protocol::status::internal;
   }
}

struct prepared_program {
   std::uint64_t id = 0;
   std::unique_ptr<backend::program> executable;
};

using program_table = std::array<prepared_program, program_limit>;

protocol::response prepare(const protocol::request& request, program_table& programs, std::uint64_t& next_id) {
   protocol::response response;
   response.sequence = request.sequence;
   const auto available =
       std::find_if(programs.begin(), programs.end(), [](const auto& item) { return !item.executable; });
   if (available == programs.end() || next_id == std::numeric_limits<std::uint64_t>::max()) {
      response.code = protocol::status::resource_exhausted;
      return response;
   }
   if (request.declarations.size() > backend::limits{}.variables) {
      response.code = protocol::status::invalid_profile;
      return response;
   }
   std::array<backend::declaration, backend::limits{}.variables> declarations;
   std::size_t count = 0;
   for (const auto& item : request.declarations) {
      backend::schema_kind kind;
      switch (item.kind) {
      case type::int64:
         kind = backend::schema_kind::int64;
         break;
      case type::uint64:
         kind = backend::schema_kind::uint64;
         break;
      case type::boolean:
         kind = backend::schema_kind::boolean;
         break;
      case type::string:
         kind = backend::schema_kind::string;
         break;
      case type::list_int64:
         kind = backend::schema_kind::list_int64;
         break;
      case type::list_uint64:
         kind = backend::schema_kind::list_uint64;
         break;
      case type::map_int64:
         kind = backend::schema_kind::map_int64;
         break;
      case type::map_uint64:
         kind = backend::schema_kind::map_uint64;
         break;
      default:
         response.code = protocol::status::invalid_profile;
         return response;
      }
      declarations[count++] = {item.name, kind};
   }
   const auto& budget = request.limits;
   const backend::limits limits{budget.source_bytes,
                                budget.nodes,
                                budget.depth,
                                budget.variables,
                                budget.collection_elements,
                                budget.string_bytes,
                                budget.total_input_bytes,
                                budget.ast_events};
   auto expected = backend::value_kind::null;
   switch (request.result) {
   case type::int64:
      expected = backend::value_kind::int64;
      break;
   case type::uint64:
      expected = backend::value_kind::uint64;
      break;
   case type::boolean:
      expected = backend::value_kind::boolean;
      break;
   default:
      break;
   }
   auto prepared =
       request.op == protocol::operation::load
           ? backend::program::load(
                 std::string_view{reinterpret_cast<const char*>(request.expression.data()), request.expression.size()},
                 std::span{declarations.data(), count}, limits, expected)
           : backend::program::prepare(request.source, std::span{declarations.data(), count}, limits);
   response.code = convert(prepared.code);
   if (response.code == protocol::status::ok) {
      if (!prepared.executable) {
         response.code = protocol::status::internal;
         return response;
      }
      response.expression.assign(prepared.expression.begin(), prepared.expression.end());
      switch (prepared.result) {
      case backend::value_kind::int64:
         response.result = type::int64;
         break;
      case backend::value_kind::uint64:
         response.result = type::uint64;
         break;
      case backend::value_kind::boolean:
         response.result = type::boolean;
         break;
      default:
         response.code = protocol::status::internal;
         return response;
      }
      available->id = next_id++;
      available->executable = std::move(prepared.executable);
      response.prepared_id = available->id;
   }
   return response;
}

protocol::response evaluate(const protocol::request& request, program_table& programs) {
   protocol::response response;
   response.sequence = request.sequence;
   const auto program = std::find_if(programs.begin(), programs.end(), [&](const auto& item) {
      return item.id == request.prepared_id && item.executable;
   });
   if (program == programs.end() || request.prepared_id == 0) {
      response.code = protocol::status::invalid_profile;
      return response;
   }
   if (request.values.size() > backend::limits{}.variables) {
      response.code = protocol::status::resource_exhausted;
      return response;
   }
   std::array<backend::named_value, backend::limits{}.variables> inputs;
   std::size_t count = 0;
   for (const auto& item : request.values) {
      inputs[count++] = {item.key(), view(item.value())};
   }
   const auto result = program->executable->evaluate(std::span{inputs.data(), count});
   response.code = convert(result.code);
   response.ast_events = result.ast_events;
   if (response.code != protocol::status::ok) {
      return response;
   }
   switch (result.kind) {
   case backend::value_kind::int64:
      response.kind = protocol::value_kind::int64;
      response.i64 = result.i64;
      break;
   case backend::value_kind::uint64:
      response.kind = protocol::value_kind::uint64;
      response.u64 = result.u64;
      break;
   case backend::value_kind::boolean:
      response.kind = protocol::value_kind::boolean;
      response.boolean = result.boolean;
      break;
   default:
      response.code = protocol::status::internal;
   }
   return response;
}

protocol::response execute(const protocol::request& request, program_table& programs, std::uint64_t& next_id,
                           bool& greeted, bool& stopped) {
   protocol::response response;
   response.sequence = request.sequence;
   if (request.op == protocol::operation::hello) {
      greeted = true;
      return response;
   }
   if (!greeted) {
      response.code = protocol::status::invalid_profile;
      return response;
   }
   switch (request.op) {
   case protocol::operation::prepare:
   case protocol::operation::load:
      return prepare(request, programs, next_id);
   case protocol::operation::evaluate:
      return evaluate(request, programs);
   case protocol::operation::release: {
      const auto found = std::find_if(programs.begin(), programs.end(), [&](const auto& item) {
         return item.id == request.prepared_id && item.executable;
      });
      if (found == programs.end() || request.prepared_id == 0) {
         response.code = protocol::status::invalid_profile;
      } else {
         found->executable.reset();
         found->id = 0;
      }
      return response;
   }
   case protocol::operation::stop:
      stopped = true;
      return response;
   case protocol::operation::memory_probe: {
      if (request.probe_bytes > std::numeric_limits<std::size_t>::max()) {
         response.code = protocol::status::invalid_input;
         return response;
      }
      const auto bytes = static_cast<std::size_t>(request.probe_bytes);
      auto* allocation = static_cast<volatile unsigned char*>(std::malloc(bytes == 0 ? 1 : bytes));
      if (!allocation) {
         response.code = protocol::status::resource_exhausted;
      } else {
         for (std::size_t offset = 0; offset < bytes; offset += 4096) {
            allocation[offset] = 1;
         }
         std::free(const_cast<unsigned char*>(allocation));
      }
      return response;
   }
   default:
      response.code = protocol::status::invalid_input;
      return response;
   }
}

void add_statistics(protocol::response& response) noexcept {
   const auto usage = memory::stats();
   response.heap_live = usage.allocated;
   response.heap_peak = usage.peak;
   response.heap_cap = usage.capacity;
   response.heap_failures = usage.failures;
}

} // namespace

int run_worker() noexcept {
   try {
      if (!memory::stats().startup_valid) {
         return 70;
      }
      program_table programs;
      std::uint64_t next_id = 1;
      std::uint64_t last_sequence = 0;
      bool greeted = false;
      bool stopped = false;
      std::array<std::uint8_t, protocol::prefix_bytes> prefix;
      std::array<std::uint8_t, protocol::frame_limit> input;
      std::array<std::uint8_t, protocol::frame_limit> output;
      while (!stopped) {
         const auto read = read_exact(prefix);
         if (read == read_result::closed) {
            return 0;
         }
         if (read != read_result::complete) {
            return 65;
         }
         const auto size = protocol::decode_length(prefix);
         if (read_exact(std::span{input}.first(size)) != read_result::complete) {
            return 65;
         }
         protocol::response response;
         {
            // Decode failures have no trustworthy exchange identity. Terminate
            // the isolated child rather than acknowledging a different request.
            auto request = protocol::decode_request(std::span{input}.first(size));
            response.sequence = request.sequence;
            if (request.sequence <= last_sequence) {
               response.code = protocol::status::invalid_input;
            } else {
               last_sequence = request.sequence;
               try {
                  response = execute(request, programs, next_id, greeted, stopped);
               } catch (const std::bad_alloc&) {
                  response.code = protocol::status::resource_exhausted;
               } catch (...) {
                  response.code = protocol::status::internal;
               }
            }
         }
         add_statistics(response);
         const auto encoded = protocol::encode_into(response, output);
         protocol::encode_length(static_cast<std::uint32_t>(encoded), prefix);
         if (!write_exact(prefix) || !write_exact(std::span{output}.first(encoded))) {
            return 74;
         }
      }
      return 0;
   } catch (const std::bad_alloc&) {
      return 75;
   } catch (...) {
      return 65;
   }
}

} // namespace forge::expression::detail
