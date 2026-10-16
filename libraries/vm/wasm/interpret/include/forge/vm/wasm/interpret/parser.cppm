module;

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module forge.vm.wasm.interpret.backend:parser;

import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.debug_info;
import forge.vm.wasm.interpret.exceptions;
import forge.vm.wasm.interpret.host_function;
import forge.vm.wasm.interpret.options;
import forge.vm.wasm.interpret.types;
import forge.vm.wasm.interpret.watchdog;
import :leb128;
import :sections;

namespace forge::vm::wasm::interpret {

namespace detail {

inline constexpr options parser_defaults{};

inline constexpr unsigned get_size_for_type(uint8_t type) {
   switch (type) {
   case types::i32:
   case types::f32:
      return 4;
   case types::i64:
   case types::f64:
      return 8;
   default:
      return 0;
   }
}

template <typename Options, typename Enable = void> struct max_mutable_globals_checker {
   constexpr void on_mutable_global(const Options&, uint8_t) {}
};

template <typename Options> using max_mutable_globals_t = decltype(std::declval<Options>().max_mutable_global_bytes);

template <typename Options> struct max_mutable_globals_checker<Options, std::void_t<max_mutable_globals_t<Options>>> {
   static_assert(std::is_unsigned_v<std::decay_t<max_mutable_globals_t<Options>>>,
                 "max_mutable_globals must be an unsigned integer type");
   void on_mutable_global(const Options& options, uint8_t type) {
      if constexpr (requires { options.check_mutable_global_bytes; }) {
         if (!options.check_mutable_global_bytes)
            return;
      }
      unsigned size = get_size_for_type(type);
      _counter += size;
      detail::check<exceptions::parse>((_counter <= options.max_mutable_global_bytes && _counter >= size),
                                       "mutable globals exceeded limit");
   }
   std::decay_t<max_mutable_globals_t<Options>> _counter = 0;
};

#define PARSER_OPTION(name, default_, type)                                                                            \
   template <typename Options> type get_##name(const Options& options, long) {                                         \
      (void)options;                                                                                                   \
      return default_;                                                                                                 \
   }                                                                                                                   \
   template <typename Options> auto get_##name(const Options& options, int) -> decltype(options.name) {                \
      return options.name;                                                                                             \
   }                                                                                                                   \
   template <typename Options> type get_##name(const Options& options) {                                               \
      return detail::get_##name(options, 0);                                                                           \
   }

#define MAX_ELEMENTS(name, default_) PARSER_OPTION(name, default_, std::uint32_t)

PARSER_OPTION(max_mutable_global_bytes, parser_defaults.max_mutable_global_bytes, std::uint64_t)
PARSER_OPTION(max_func_local_bytes, parser_defaults.max_func_local_bytes, std::uint64_t)
PARSER_OPTION(check_mutable_global_bytes, parser_defaults.check_mutable_global_bytes, bool)
PARSER_OPTION(check_func_local_bytes, parser_defaults.check_func_local_bytes, bool)
PARSER_OPTION(check_control_depth, parser_defaults.check_control_depth, bool)
PARSER_OPTION(check_legacy_nested_structures, parser_defaults.check_legacy_nested_structures, bool)

MAX_ELEMENTS(max_table_elements, parser_defaults.max_table_elements)
MAX_ELEMENTS(max_section_elements, parser_defaults.max_section_elements)

MAX_ELEMENTS(max_type_section_elements, detail::get_max_section_elements(options))
MAX_ELEMENTS(max_import_section_elements, detail::get_max_section_elements(options))
MAX_ELEMENTS(max_function_section_elements, detail::get_max_section_elements(options))
MAX_ELEMENTS(max_global_section_elements, detail::get_max_section_elements(options))
MAX_ELEMENTS(max_export_section_elements, detail::get_max_section_elements(options))
MAX_ELEMENTS(max_element_section_elements, detail::get_max_section_elements(options))
MAX_ELEMENTS(max_data_section_elements, detail::get_max_section_elements(options))

MAX_ELEMENTS(max_element_segment_elements, parser_defaults.max_element_segment_elements)
MAX_ELEMENTS(max_data_segment_bytes, parser_defaults.max_data_segment_bytes)

PARSER_OPTION(max_linear_memory_init, parser_defaults.max_linear_memory_init, std::uint64_t)
PARSER_OPTION(max_func_local_bytes_flags, parser_defaults.max_func_local_bytes_flags, max_func_local_bytes_flags_t);

template <typename Options, typename Enable = void> struct max_func_local_bytes_checker {
   explicit max_func_local_bytes_checker(const Options&, const func_type& /*ft*/) {}
   void on_local(const Options&, std::uint8_t, const std::uint32_t) {}
   void push_stack(const Options& /*options*/, std::uint8_t /*type*/) {}
   void pop_stack(std::uint8_t /*type*/) {}
   void push_unreachable() {}
   void pop_unreachable() {}
   static constexpr bool is_defined = false;
};
template <typename Options>
struct max_func_local_bytes_checker<Options, std::void_t<decltype(std::declval<Options>().max_func_local_bytes)>> {
   explicit max_func_local_bytes_checker(const Options& options, const func_type& ft) {
      if constexpr (requires { options.check_func_local_bytes; }) {
         if (!options.check_func_local_bytes)
            return;
      }
      if ((detail::get_max_func_local_bytes_flags(options) & max_func_local_bytes_flags_t::params) !=
          (max_func_local_bytes_flags_t)0) {
         for (std::uint32_t i = 0; i < ft.param_types.size(); ++i) {
            on_type(options, ft.param_types.at(i));
         }
      }
   }
   void on_type(const Options& options, std::uint8_t type) {
      if constexpr (requires { options.check_func_local_bytes; }) {
         if (!options.check_func_local_bytes)
            return;
      }
      unsigned size = get_size_for_type(type);
      _count += size;
      detail::check<exceptions::parse>((_count <= options.max_func_local_bytes && _count >= size),
                                       "local variable limit exceeded");
   }
   void on_local(const Options& options, std::uint8_t type, std::uint32_t count) {
      if constexpr (requires { options.check_func_local_bytes; }) {
         if (!options.check_func_local_bytes)
            return;
      }
      if ((detail::get_max_func_local_bytes_flags(options) & max_func_local_bytes_flags_t::locals) !=
          (max_func_local_bytes_flags_t)0) {
         uint64_t size = get_size_for_type(type);
         size *= count;
         _count += size;
         detail::check<exceptions::parse>((_count <= options.max_func_local_bytes && _count >= size),
                                          "local variable limit exceeded");
      }
   }
   std::decay_t<decltype(std::declval<Options>().max_func_local_bytes)> _count = 0;
   static constexpr bool is_defined = true;
};
template <typename Options>
constexpr auto get_max_func_local_bytes_no_stack_c(int)
    -> std::enable_if_t<std::is_pointer_v<decltype(&Options::max_func_local_bytes_flags)>, bool> {
   return (Options::max_func_local_bytes_flags & max_func_local_bytes_flags_t::stack) ==
          (max_func_local_bytes_flags_t)0;
}
template <typename Options> constexpr auto get_max_func_local_bytes_no_stack_c(long) -> bool {
   return false;
}

template <typename Options, typename Enable = void>
struct max_func_local_bytes_stack_checker : max_func_local_bytes_checker<Options> {
   explicit constexpr max_func_local_bytes_stack_checker(const max_func_local_bytes_checker<Options>& base)
       : max_func_local_bytes_checker<Options>(base) {}
   void push_stack(const Options& options, std::uint8_t type) {
      if constexpr (requires { options.check_func_local_bytes; }) {
         if (!options.check_func_local_bytes)
            return;
      }
      if (unreachable_depth == 0 && (detail::get_max_func_local_bytes_flags(options) &
                                     max_func_local_bytes_flags_t::stack) != (max_func_local_bytes_flags_t)0) {
         this->on_type(options, type);
      }
   }
   void pop_stack(const Options& options, std::uint8_t type) {
      if constexpr (requires { options.check_func_local_bytes; }) {
         if (!options.check_func_local_bytes)
            return;
      }
      if (unreachable_depth == 0 && (detail::get_max_func_local_bytes_flags(options) &
                                     max_func_local_bytes_flags_t::stack) != (max_func_local_bytes_flags_t)0) {
         this->_count -= get_size_for_type(type);
      }
   }
   void push_unreachable() {
      ++unreachable_depth;
   }
   void pop_unreachable() {
      --unreachable_depth;
   }
   std::uint32_t unreachable_depth = 0;
};
template <typename Options>
struct max_func_local_bytes_stack_checker<Options,
                                          std::enable_if_t<!max_func_local_bytes_checker<Options>::is_defined ||
                                                           get_max_func_local_bytes_no_stack_c<Options>(0)>> {
   explicit constexpr max_func_local_bytes_stack_checker(const max_func_local_bytes_checker<Options>&) {}
   void push_stack(const Options& /*options*/, std::uint8_t /*type*/) {}
   void pop_stack(const Options& /*options*/, std::uint8_t /*type*/) {}
   void push_unreachable() {}
   void pop_unreachable() {}
};

MAX_ELEMENTS(max_local_sets, parser_defaults.max_local_sets)
MAX_ELEMENTS(max_nested_structures, parser_defaults.max_nested_structures)
MAX_ELEMENTS(max_br_table_elements, parser_defaults.max_br_table_elements)
PARSER_OPTION(max_control_depth, parser_defaults.max_control_depth, std::uint64_t)
PARSER_OPTION(max_legacy_nested_structures, parser_defaults.max_legacy_nested_structures, std::uint64_t)

template <typename Options, typename Enable = void> struct max_control_depth_checker {
   void on_control(const Options&) {}
   void on_end(const Options&) {}
};
template <typename Options>
struct max_control_depth_checker<Options, std::void_t<decltype(std::declval<Options>().max_control_depth)>> {
   void on_control(const Options& options) {
      if constexpr (requires { options.check_control_depth; }) {
         if (!options.check_control_depth)
            return;
      }
      ++_count;
      detail::check<exceptions::parse>((_count <= options.max_control_depth), "Nested depth exceeded");
   }
   void on_end(const Options& options) {
      if constexpr (requires { options.check_control_depth; }) {
         if (!options.check_control_depth)
            return;
      }
      detail::check<exceptions::parse>((_count > 0), "control depth underflow");
      --_count;
   }
   std::decay_t<decltype(std::declval<Options>().max_control_depth)> _count = 0;
};

// Preserves the donor compatibility profile, which counts an `else` as a
// nested structure even though it does not increase WebAssembly control depth.
template <typename Options, typename Enable = void> struct max_legacy_nested_structures_checker {
   void on_control(const Options&) {}
   void on_end(const Options&) {}
};
template <typename Options>
struct max_legacy_nested_structures_checker<
    Options, std::void_t<decltype(std::declval<Options>().max_legacy_nested_structures)>> {
   void on_control(const Options& options) {
      if constexpr (requires { options.check_legacy_nested_structures; }) {
         if (!options.check_legacy_nested_structures)
            return;
      }
      ++_count;
      detail::check<exceptions::parse>((_count <= options.max_legacy_nested_structures), "Nested depth exceeded");
   }
   void on_end(const Options& options) {
      if constexpr (requires { options.check_legacy_nested_structures; }) {
         if (!options.check_legacy_nested_structures)
            return;
      }
      if (_count == 0)
         ++_count;
      else
         --_count;
   }
   std::decay_t<decltype(std::declval<Options>().max_legacy_nested_structures)> _count = 0;
};

MAX_ELEMENTS(max_symbol_bytes, parser_defaults.max_symbol_bytes)
MAX_ELEMENTS(max_memory_offset, parser_defaults.max_memory_offset)
MAX_ELEMENTS(max_code_bytes, parser_defaults.max_code_bytes)
MAX_ELEMENTS(max_pages, parser_defaults.max_pages)
MAX_ELEMENTS(max_call_depth, parser_defaults.max_call_depth)

PARSER_OPTION(forbid_export_mutable_globals, parser_defaults.forbid_export_mutable_globals, bool);
PARSER_OPTION(allow_code_after_function_end, parser_defaults.allow_code_after_function_end, bool);
PARSER_OPTION(allow_u32_limits_flags, parser_defaults.allow_u32_limits_flags, bool);
PARSER_OPTION(allow_invalid_empty_local_set, parser_defaults.allow_invalid_empty_local_set, bool);

PARSER_OPTION(allow_zero_blocktype, parser_defaults.allow_zero_blocktype, bool)

PARSER_OPTION(parse_custom_section_name, parser_defaults.parse_custom_section_name, bool);

#undef MAX_ELEMENTS
#undef PARSER_OPTION

} // namespace detail

template <typename Writer, typename Options = default_options, typename DebugInfo = null_debug_info>
class binary_parser {
 public:
   explicit binary_parser(growable_allocator& alloc, const Options& options = Options{})
       : _allocator(alloc), _options(options) {}

   template <typename T> using vec = guarded_vector<T>;

   static inline uint8_t parse_varuint1(wasm_code_ptr& code) {
      return varuint<1>(code).to();
   }

   static inline uint8_t parse_varuint7(wasm_code_ptr& code) {
      return varuint<7>(code).to();
   }

   static inline uint32_t parse_varuint32(wasm_code_ptr& code) {
      return varuint<32>(code).to();
   }

   static inline int8_t parse_varint7(wasm_code_ptr& code) {
      return varint<7>(code).to();
   }

   static inline int32_t parse_varint32(wasm_code_ptr& code) {
      return varint<32>(code).to();
   }

   static inline int64_t parse_varint64(wasm_code_ptr& code) {
      return varint<64>(code).to();
   }

   int validate_utf8_code_point(wasm_code_ptr& code) {
      unsigned char ch = *code++;
      if (ch < 0x80) {
         return 1;
      } else if (ch < 0xE0) {
         detail::check<exceptions::parse>(((ch & 0xC0) == 0xC0), "invalid utf8 encoding");
         unsigned char b2 = *code++;
         detail::check<exceptions::parse>(((b2 & 0xC0) == 0x80), "invalid utf8 encoding");
         uint32_t code_point = (static_cast<uint32_t>(ch - 0xC0u) << 6u) + (static_cast<uint32_t>(b2 - 0x80u));
         detail::check<exceptions::parse>((0x80 <= code_point && code_point < 0x800), "invalid utf8 encoding");
         return 2;
      } else if (ch < 0xF0) {
         unsigned char b2 = *code++;
         detail::check<exceptions::parse>(((b2 & 0xC0) == 0x80), "invalid utf8 encoding");
         unsigned char b3 = *code++;
         detail::check<exceptions::parse>(((b3 & 0xC0) == 0x80), "invalid utf8 encoding");
         uint32_t code_point = (static_cast<uint32_t>(ch - 0xE0u) << 12u) + (static_cast<uint32_t>(b2 - 0x80u) << 6u) +
                               (static_cast<uint32_t>(b3 - 0x80u));
         detail::check<exceptions::parse>(
             ((0x800 <= code_point && code_point < 0xD800) || (0xE000 <= code_point && code_point < 0x10000)),
             "invalid utf8 encoding");
         return 3;
      } else if (ch < 0xF8) {
         unsigned char b2 = *code++;
         detail::check<exceptions::parse>(((b2 & 0xC0) == 0x80), "invalid utf8 encoding");
         unsigned char b3 = *code++;
         detail::check<exceptions::parse>(((b3 & 0xC0) == 0x80), "invalid utf8 encoding");
         unsigned char b4 = *code++;
         detail::check<exceptions::parse>(((b4 & 0xC0) == 0x80), "invalid utf8 encoding");
         uint32_t code_point = (static_cast<uint32_t>(ch - 0xF0u) << 18u) + (static_cast<uint32_t>(b2 - 0x80u) << 12u) +
                               (static_cast<uint32_t>(b3 - 0x80u) << 6u) + (static_cast<uint32_t>(b4 - 0x80u));
         detail::check<exceptions::parse>(((0x10000 <= code_point && code_point < 0x110000)), "invalid utf8 encoding");
         return 4;
      }
      detail::fail<exceptions::parse>("invalid utf8 encoding");
   }

   void validate_utf8_string(wasm_code_ptr& code, uint32_t bytes) {
      while (bytes != 0) {
         bytes -= validate_utf8_code_point(code);
      }
   }

   guarded_vector<uint8_t> parse_utf8_string(wasm_code_ptr& code, std::uint32_t max_size) {
      auto len = parse_varuint32(code);
      detail::check<exceptions::parse>((len <= max_size), "name too long");
      auto guard = code.scoped_shrink_bounds(len);
      auto result = guarded_vector<uint8_t>{_allocator, len};
      result.copy(code.raw(), len);
      validate_utf8_string(code, len);
      return result;
   }

   template <typename T> T parse_raw(wasm_code_ptr& code) {
      static_assert(std::is_arithmetic_v<T>, "Can only read builtin types");
      auto guard = code.scoped_shrink_bounds(sizeof(T));
      T result;
      std::memcpy(&result, code.raw(), sizeof(T));
      code += sizeof(T);
      return result;
   }

   inline module& parse_module(wasm_code& code, module& mod, DebugInfo& debug) {
      wasm_code_ptr cp(code.data(), code.size());
      parse_module(cp, code.size(), mod, debug);
      return mod;
   }

   inline module& parse_module2(wasm_code_ptr& code_ptr, size_t sz, module& mod, DebugInfo& debug) {
      parse_module(code_ptr, sz, mod, debug);
      return mod;
   }

   void parse_module(wasm_code_ptr& code_ptr, size_t sz, module& mod, DebugInfo& debug) {
      _mod = &mod;
      detail::check<exceptions::parse>((parse_magic(code_ptr) == constants::magic), "magic number did not match");
      detail::check<exceptions::parse>((parse_version(code_ptr) == constants::version), "version number did not match");
      uint8_t highest_section_id = 0;
      for (;;) {
         if (code_ptr.offset() == sz)
            break;
         auto id = parse_section_id(code_ptr);
         auto len = parse_section_payload_len(code_ptr);

         detail::check<exceptions::parse>((id == 0 || id > highest_section_id), "section out of order");
         highest_section_id = std::max(highest_section_id, id);

         auto section_guard = code_ptr.scoped_consume_items(len);

         switch (id) {
         case section_id::custom_section:
            parse_custom(code_ptr);
            break;
         case section_id::type_section:
            parse_section<section_id::type_section>(code_ptr, mod.types);
            break;
         case section_id::import_section:
            parse_section<section_id::import_section>(code_ptr, mod.imports);
            break;
         case section_id::function_section:
            parse_section<section_id::function_section>(code_ptr, mod.functions);
            break;
         case section_id::table_section:
            parse_section<section_id::table_section>(code_ptr, mod.tables);
            break;
         case section_id::memory_section:
            parse_section<section_id::memory_section>(code_ptr, mod.memories);
            break;
         case section_id::global_section:
            parse_section<section_id::global_section>(code_ptr, mod.globals);
            break;
         case section_id::export_section:
            parse_section<section_id::export_section>(code_ptr, mod.exports);
            validate_exports();
            break;
         case section_id::start_section:
            parse_section<section_id::start_section>(code_ptr, mod.start);
            break;
         case section_id::element_section:
            parse_section<section_id::element_section>(code_ptr, mod.elements);
            break;
         case section_id::code_section:
            mod.normalize_types();
            parse_section<section_id::code_section>(code_ptr, mod.code);
            break;
         case section_id::data_section:
            parse_section<section_id::data_section>(code_ptr, mod.data);
            break;
         default:
            detail::fail<exceptions::parse>("error invalid section id");
         }
      }
      detail::check<exceptions::parse>((_mod->code.size() == _mod->functions.size()),
                                       "code section must have the same size as the function section");

      debug.set(std::move(imap));
      debug.relocate(_allocator.get_code_start());
   }

   inline uint32_t parse_magic(wasm_code_ptr& code) {
      return parse_raw<uint32_t>(code);
   }
   inline uint32_t parse_version(wasm_code_ptr& code) {
      return parse_raw<uint32_t>(code);
   }
   inline uint8_t parse_section_id(wasm_code_ptr& code) {
      return *code++;
   }
   inline uint32_t parse_section_payload_len(wasm_code_ptr& code) {
      return parse_varuint32(code);
   }

   inline void parse_custom(wasm_code_ptr& code) {
      auto section_name = parse_utf8_string(code, 0xFFFFFFFFu); // ignored, but needs to be validated
      if (detail::get_parse_custom_section_name(_options) && section_name.size() == 4 &&
          std::memcmp(section_name.raw(), "name", 4) == 0) {
         parse_name_section(code);
      } else {
         // skip to the end of the section
         code += code.bounds() - code.offset();
      }
   }

   inline void parse_name_map(wasm_code_ptr& code, guarded_vector<name_assoc>& map) {
      for (uint32_t i = 0; i < map.size(); ++i) {
         map[i].idx = parse_varuint32(code);
         map[i].name = parse_utf8_string(code, 0xFFFFFFFFu);
      }
   }

   inline void parse_name_section(wasm_code_ptr& code) {
      _mod->names = _allocator.alloc<name_section>(1);
      new (_mod->names) name_section;
      if (code.bounds() == code.offset())
         return;
      if (*code == 0) {
         ++code;
         auto subsection_guard = code.scoped_consume_items(parse_varuint32(code));
         _mod->names->module_name = _allocator.alloc<guarded_vector<uint8_t>>(1);
         new (_mod->names->module_name) guarded_vector<uint8_t>(parse_utf8_string(code, 0xFFFFFFFFu));
      }
      if (code.bounds() == code.offset())
         return;
      if (*code == 1) {
         ++code;
         auto subsection_guard = code.scoped_consume_items(parse_varuint32(code));
         uint32_t size = parse_varuint32(code);
         _mod->names->function_names = _allocator.alloc<guarded_vector<name_assoc>>(1);
         new (_mod->names->function_names) guarded_vector<name_assoc>(_allocator, size);
         parse_name_map(code, *_mod->names->function_names);
      }
      if (code.bounds() == code.offset())
         return;
      if (*code == 2) {
         ++code;
         auto subsection_guard = code.scoped_consume_items(parse_varuint32(code));
         uint32_t size = parse_varuint32(code);
         _mod->names->local_names = _allocator.alloc<guarded_vector<indirect_name_assoc>>(1);
         new (_mod->names->local_names) guarded_vector<indirect_name_assoc>(_allocator, size);
         for (uint32_t i = 0; i < size; ++i) {
            auto& [idx, namemap] = (*_mod->names->local_names)[i];
            idx = parse_varuint32(code);
            uint32_t local_size = parse_varuint32(code);
            namemap = guarded_vector<name_assoc>(_allocator, local_size);
            parse_name_map(code, namemap);
         }
      }
      if (code.bounds() == code.offset())
         return;
      detail::fail<exceptions::parse>("Invalid subsection Id");
   }

   void parse_import_entry(wasm_code_ptr& code, import_entry& entry) {
      entry.module_str = parse_utf8_string(code, detail::get_max_symbol_bytes(_options));
      entry.field_str = parse_utf8_string(code, detail::get_max_symbol_bytes(_options));
      entry.kind = (external_kind)(*code++);
      auto type = parse_varuint32(code);
      switch ((uint8_t)entry.kind) {
      case external_kind::Function:
         entry.type.func_t = type;
         detail::check<exceptions::parse>((type < _mod->types.size()), "Invalid function type");
         break;
      default:
         detail::fail<exceptions::unsupported_import>("only function imports are supported");
      }
   }

   uint8_t parse_flags(wasm_code_ptr& code) {
      if (detail::get_allow_u32_limits_flags(_options)) {
         return parse_varuint32(code) & 0x1;
      } else {
         detail::check<exceptions::parse>((*code == 0x0 || *code == 0x1), "invalid flags");
         return *code++;
      }
   }

   void parse_table_type(wasm_code_ptr& code, table_type& tt) {
      tt.element_type = *code++;
      detail::check<exceptions::parse>((tt.element_type == types::anyfunc), "table must have type anyfunc");
      tt.limits.flags = parse_flags(code);
      tt.limits.initial = parse_varuint32(code);
      if (tt.limits.flags) {
         tt.limits.maximum = parse_varuint32(code);
         detail::check<exceptions::parse>((tt.limits.initial <= tt.limits.maximum),
                                          "table max size less than min size");
      }
      detail::check<exceptions::parse>((tt.limits.initial <= detail::get_max_table_elements(_options)),
                                       "table size exceeds limit");
      tt.table = decltype(tt.table){_allocator, tt.limits.initial};
      for (uint32_t i = 0; i < tt.limits.initial; i++)
         tt.table[i] = std::numeric_limits<uint32_t>::max();
   }

   void parse_global_variable(wasm_code_ptr& code, global_variable& gv) {
      uint8_t ct = *code++;
      gv.type.content_type = ct;
      detail::check<exceptions::parse>((ct == types::i32 || ct == types::i64 || ct == types::f32 || ct == types::f64),
                                       "invalid global content type");

      gv.type.mutability = parse_varuint1(code);
      if (gv.type.mutability)
         on_mutable_global(ct);
      parse_init_expr(code, gv.init, ct);
   }

   void parse_memory_type(wasm_code_ptr& code, memory_type& mt) {
      mt.limits.flags = parse_flags(code);
      mt.limits.initial = parse_varuint32(code);
      // Implementation limits
      detail::check<exceptions::parse>((mt.limits.initial <= detail::get_max_pages(_options)),
                                       "initial memory out of range");
      // WASM specification
      detail::check<exceptions::parse>((mt.limits.initial <= 65536u), "initial memory out of range");
      if (mt.limits.flags) {
         mt.limits.maximum = parse_varuint32(code);
         detail::check<exceptions::parse>((mt.limits.maximum >= mt.limits.initial), "maximum must be at least minimum");
         detail::check<exceptions::parse>((mt.limits.maximum <= 65536u), "maximum memory out of range");
      }
   }

   void parse_export_entry(wasm_code_ptr& code, export_entry& entry) {
      entry.field_str = parse_utf8_string(code, detail::get_max_symbol_bytes(_options));
      entry.kind = (external_kind)(*code++);
      entry.index = parse_varuint32(code);
      switch (entry.kind) {
      case external_kind::Function:
         detail::check<exceptions::parse>((entry.index < _mod->get_functions_total()), "function export out of range");
         break;
      case external_kind::Table:
         detail::check<exceptions::parse>((entry.index < _mod->tables.size()), "table export out of range");
         break;
      case external_kind::Memory:
         detail::check<exceptions::parse>((entry.index < _mod->memories.size()), "memory export out of range");
         break;
      case external_kind::Global:
         detail::check<exceptions::parse>((entry.index < _mod->globals.size()), "global export out of range");
         detail::check<exceptions::parse>(
             (!detail::get_forbid_export_mutable_globals(_options) || !_mod->globals.at(entry.index).type.mutability),
             "cannot export mutable globals");
         break;
      default:
         detail::fail<exceptions::parse>("Unknown export kind");
         break;
      }
   }

   void parse_func_type(wasm_code_ptr& code, func_type& ft) {
      ft.form = *code++;
      detail::check<exceptions::parse>((ft.form == 0x60), "invalid function type");
      decltype(ft.param_types) param_types = {_allocator, parse_varuint32(code)};
      for (size_t i = 0; i < param_types.size(); i++) {
         uint8_t pt = *code++;
         param_types.at(i) = pt;
         detail::check<exceptions::parse>(
             (pt == types::i32 || pt == types::i64 || pt == types::f32 || pt == types::f64),
             "invalid function param type");
      }
      ft.param_types = std::move(param_types);
      ft.return_count = *code++;
      detail::check<exceptions::parse>((ft.return_count < 2), "invalid function return count");
      if (ft.return_count > 0) {
         uint8_t rt = *code++;
         ft.return_type = rt;
         detail::check<exceptions::parse>(
             (rt == types::i32 || rt == types::i64 || rt == types::f32 || rt == types::f64),
             "invalid function return type");
      }
   }

   void parse_elem_segment(wasm_code_ptr& code, elem_segment& es) {
      table_type* tt = nullptr;
      for (std::size_t i = 0; i < _mod->tables.size(); i++) {
         if (_mod->tables[i].element_type == types::anyfunc)
            tt = &(_mod->tables[i]);
      }
      detail::check<exceptions::parse>((tt != nullptr), "table not declared");
      es.index = parse_varuint32(code);
      detail::check<exceptions::parse>((es.index == 0), "only table index of 0 is supported");
      parse_init_expr(code, es.offset, types::i32);
      uint32_t size = parse_varuint32(code);
      detail::check<exceptions::parse>((size <= detail::get_max_element_segment_elements(_options)),
                                       "elem segment too large");
      decltype(es.elems) elems = {_allocator, size};
      for (uint32_t i = 0; i < size; i++) {
         uint32_t index = parse_varuint32(code);
         elems.at(i) = index;
         detail::check<exceptions::parse>((index < _mod->get_functions_total()), "elem for undefined function");
      }
      uint32_t offset = static_cast<uint32_t>(es.offset.value.i32);
      if (static_cast<uint64_t>(size) + offset <= tt->table.size()) {
         if (size > 0)
            std::memcpy(tt->table.raw() + offset, elems.raw(), size * sizeof(uint32_t));
      } else {
         _mod->error = "elem out of range";
      }
      es.elems = std::move(elems);
   }

   void parse_init_expr(wasm_code_ptr& code, init_expr& ie, uint8_t type) {
      ie.opcode = *code++;
      switch (ie.opcode) {
      case opcodes::i32_const:
         ie.value.i32 = parse_varint32(code);
         detail::check<exceptions::parse>((type == types::i32), "expected i32 initializer");
         break;
      case opcodes::i64_const:
         ie.value.i64 = parse_varint64(code);
         detail::check<exceptions::parse>((type == types::i64), "expected i64 initializer");
         break;
      case opcodes::f32_const:
         ie.value.f32 = parse_raw<uint32_t>(code);
         detail::check<exceptions::parse>((type == types::f32), "expected f32 initializer");
         break;
      case opcodes::f64_const:
         ie.value.f64 = parse_raw<uint64_t>(code);
         detail::check<exceptions::parse>((type == types::f64), "expected f64 initializer");
         break;
      default:
         detail::fail<exceptions::parse>(
             "initializer expression can only acception i32.const, i64.const, f32.const and f64.const");
      }
      detail::check<exceptions::parse>(((*code++) == opcodes::end), "no end op found");
   }

   void parse_function_body(wasm_code_ptr& code, function_body& fb, std::size_t idx) {
      fb.size = parse_varuint32(code);
      detail::check<exceptions::parse>((fb.size <= detail::get_max_code_bytes(_options)), "Function body too large");
      const auto& before = code.offset();
      const auto& local_cnt = parse_varuint32(code);
      _current_function_index++;
      detail::check<exceptions::parse>((local_cnt <= detail::get_max_local_sets(_options)),
                                       "Number of local sets exceeds limit");
      decltype(fb.locals) locals = {_allocator, local_cnt};
      func_type& ft = _mod->types.at(_mod->functions.at(idx));
      detail::max_func_local_bytes_checker<Options> local_checker(_options, ft);
      // parse the local entries
      for (size_t i = 0; i < local_cnt; i++) {
         auto count = parse_varuint32(code);
         auto type = *code++;
         if (detail::get_allow_invalid_empty_local_set(_options) && count == 0)
            type = types::i32;
         detail::check<exceptions::parse>(
             (type == types::i32 || type == types::i64 || type == types::f32 || type == types::f64),
             "invalid local type");
         local_checker.on_local(_options, type, count);
         locals.at(i).count = count;
         locals.at(i).type = type;
      }
      fb.locals = std::move(locals);

      fb.size -= code.offset() - before;
      auto guard = code.scoped_shrink_bounds(fb.size);
      _function_bodies.emplace_back(wasm_code_ptr{code.raw(), fb.size}, local_checker);

      code += fb.size - 1;
      detail::check<exceptions::parse>((detail::get_allow_code_after_function_end(_options) || *code == 0x0B),
                                       "failed parsing function body, expected 'end'");
      ++code;
   }

   // The control stack holds either address of the target of the
   // label (for backward jumps) or a list of instructions to be
   // updated (for forward jumps).
   //
   // Inside an if: The first element refers to the `if` and should
   // jump to `else`.  The remaining elements should branch to `end`
   using label_t = decltype(std::declval<Writer>().emit_end());
   using branch_t = decltype(std::declval<Writer>().emit_if());
   struct pc_element_t {
      uint32_t operand_depth;
      uint32_t expected_result;
      uint32_t label_result;
      bool is_if;
      std::variant<label_t, std::vector<branch_t>> relocations;
   };

   static constexpr uint8_t any_type = 0x82;
   struct operand_stack_type_tracker {
      explicit operand_stack_type_tracker(const detail::max_func_local_bytes_stack_checker<Options> local_bytes_checker,
                                          const Options& options)
          : local_bytes_checker(local_bytes_checker), options(options) {}
      std::vector<uint8_t> state = {scope_tag};
      static constexpr uint8_t unreachable_tag = 0x80;
      static constexpr uint8_t scope_tag = 0x81;
      uint32_t operand_depth = 0;
      uint32_t maximum_operand_depth = 0;
      detail::max_func_local_bytes_stack_checker<Options> local_bytes_checker;
      const Options& options;
      void push(uint8_t type) {
         assert(type != unreachable_tag && type != scope_tag);
         assert(type == types::i32 || type == types::i64 || type == types::f32 || type == types::f64 ||
                type == any_type);
         detail::check<exceptions::parse>((operand_depth < std::numeric_limits<uint32_t>::max()),
                                          "integer overflow in operand depth");
         ++operand_depth;
         maximum_operand_depth = std::max(operand_depth, maximum_operand_depth);
         state.push_back(type);
         local_bytes_checker.push_stack(options, type);
      }
      void pop(uint8_t expected) {
         assert(expected != unreachable_tag && expected != scope_tag);
         if (expected == types::pseudo)
            return;
         detail::check<exceptions::parse>((!state.empty()), "unexpected pop");
         if (state.back() != unreachable_tag) {
            detail::check<exceptions::parse>((state.back() == expected || state.back() == any_type), "wrong type");
            local_bytes_checker.pop_stack(options, expected);
            --operand_depth;
            state.pop_back();
         }
      }
      uint8_t pop() {
         detail::check<exceptions::parse>((!state.empty() && state.back() != scope_tag), "unexpected pop");
         if (state.back() == unreachable_tag)
            return any_type;
         else {
            uint8_t result = state.back();
            --operand_depth;
            local_bytes_checker.pop_stack(options, result);
            state.pop_back();
            return result;
         }
      }
      void top(uint8_t expected) {
         // Constrain the top of the stack if it was any_type or unreachable_tag.
         pop(expected);
         push(expected);
      }
      void start_unreachable() {
         while (!state.empty() && state.back() != scope_tag) {
            if (state.back() != unreachable_tag) {
               local_bytes_checker.pop_stack(options, state.back());
               --operand_depth;
            }
            state.pop_back();
         }
         local_bytes_checker.push_unreachable();
         state.push_back(unreachable_tag);
      }
      void push_scope() {
         state.push_back(scope_tag);
      }
      void pop_scope(uint8_t expected_result = types::pseudo) {
         pop(expected_result);
         detail::check<exceptions::parse>((!state.empty()), "unexpected end");
         if (state.back() == unreachable_tag) {
            local_bytes_checker.pop_unreachable();
            state.pop_back();
         }
         detail::check<exceptions::parse>((state.back() == scope_tag), "unexpected end");
         state.pop_back();
         if (expected_result != types::pseudo) {
            push(expected_result);
         }
      }
      void finish() {
         if (!state.empty() && state.back() == unreachable_tag) {
            state.pop_back();
         }
         detail::check<exceptions::parse>((state.empty()), "stack not empty at scope end");
      }
      uint32_t depth() const {
         return operand_depth;
      }
   };

   struct local_types_t {
      local_types_t(const func_type& ft, const guarded_vector<local_entry>& locals_arg) : _ft(ft), _locals(locals_arg) {
         uint32_t count = ft.param_types.size();
         _boundaries.push_back(count);
         for (uint32_t i = 0; i < locals_arg.size(); ++i) {
            // This test cannot overflow.
            detail::check<exceptions::parse>((count <= 0xFFFFFFFFu - locals_arg[i].count), "too many locals");
            count += locals_arg[i].count;
            _boundaries.push_back(count);
         }
      }
      uint8_t operator[](uint32_t local_idx) const {
         detail::check<exceptions::parse>((local_idx < _boundaries.back()), "undefined local");
         auto pos = std::upper_bound(_boundaries.begin(), _boundaries.end(), local_idx);
         if (pos == _boundaries.begin())
            return _ft.param_types[local_idx];
         else
            return _locals[pos - _boundaries.begin() - 1].type;
      }
      uint64_t locals_count() const {
         uint64_t total = _boundaries.back();
         return total - _ft.param_types.size();
      }
      const func_type& _ft;
      const guarded_vector<local_entry>& _locals;
      std::vector<uint32_t> _boundaries;
   };

   void parse_function_body_code(wasm_code_ptr& code, size_t bounds,
                                 const detail::max_func_local_bytes_stack_checker<Options>& local_bytes_checker,
                                 Writer& code_writer, const func_type& ft, const local_types_t& local_types) {

      // Initialize the control stack with the current function as the sole element
      operand_stack_type_tracker op_stack{local_bytes_checker, _options};
      std::vector<pc_element_t> pc_stack{
          {op_stack.depth(), ft.return_count ? ft.return_type : static_cast<uint32_t>(types::pseudo),
           ft.return_count ? ft.return_type : static_cast<uint32_t>(types::pseudo), false, std::vector<branch_t>{}}};

      // writes the continuation of a label to address.  If the continuation
      // is not yet available, address will be recorded in the relocations
      // list for label.
      auto handle_branch_target = [&](uint32_t label, branch_t address) {
         detail::check<exceptions::parse>((label < pc_stack.size()), "invalid label");
         pc_element_t& branch_target = pc_stack[pc_stack.size() - label - 1];
         std::visit(overloaded{[&](label_t target) { code_writer.fix_branch(address, target); },
                               [&](std::vector<branch_t>& relocations) { relocations.push_back(address); }},
                    branch_target.relocations);
      };

      // Returns the number of operands that need to be popped when
      // branching to label.  If the label has a return value it will
      // be counted in this, and the high bit will be set to signal
      // its presence.
      auto compute_depth_change = [&](uint32_t label) -> uint32_t {
         detail::check<exceptions::parse>((label < pc_stack.size()), "invalid label");
         pc_element_t& branch_target = pc_stack[pc_stack.size() - label - 1];
         uint32_t result = op_stack.depth() - branch_target.operand_depth;
         if (branch_target.label_result != types::pseudo) {
            // FIXME: Reusing the high bit imposes an additional constraint
            // on the maximum depth of the operand stack.  This isn't an
            // actual problem right now, because the stack is hard-coded
            // to 8192 elements, but it would be better to avoid spreading
            // this assumption around the code.
            result |= 0x80000000;
            op_stack.top(branch_target.label_result);
         }
         return result;
      };

      // Handles branches to the end of the scope and pops the pc_stack
      auto exit_scope = [&]() {
         // There must be at least one element
         detail::check<exceptions::parse>((pc_stack.size()), "unexpected end instruction");
         // an if with an empty else cannot have a return value
         detail::check<exceptions::parse>((!pc_stack.back().is_if || pc_stack.back().expected_result == types::pseudo),
                                          "wrong type");
         auto end_pos = code_writer.emit_end();
         if (auto* relocations = std::get_if<std::vector<branch_t>>(&pc_stack.back().relocations)) {
            for (auto branch_op : *relocations) {
               code_writer.fix_branch(branch_op, end_pos);
            }
         }
         op_stack.pop_scope(pc_stack.back().expected_result);
         pc_stack.pop_back();
      };

      auto check_in_bounds = [&] {
         detail::check<exceptions::parse>((!detail::get_allow_code_after_function_end(_options) || !pc_stack.empty()),
                                          "code after function end");
      };

      while (code.offset() < bounds) {
         detail::check<exceptions::parse>((pc_stack.size() <= detail::get_max_nested_structures(_options)),
                                          "nested structures validation failure");

         imap.on_instr_start(code_writer.get_addr(), code.raw());

         switch (*code++) {
         case opcodes::unreachable:
            check_in_bounds();
            code_writer.emit_unreachable();
            op_stack.start_unreachable();
            break;
         case opcodes::nop:
            code_writer.emit_nop();
            break;
         case opcodes::end: {
            check_in_bounds();
            const bool closes_control = pc_stack.size() > 1;
            exit_scope();
            detail::check<exceptions::parse>(
                (detail::get_allow_code_after_function_end(_options) || !pc_stack.empty() || code.offset() == bounds),
                "function too short");
            if (closes_control)
               _control_depth_checker.on_end(_options);
            _compatibility_nested_checker.on_end(_options);
            break;
         }
         case opcodes::return_: {
            check_in_bounds();
            uint32_t label = pc_stack.size() - 1;
            auto branch = code_writer.emit_return(compute_depth_change(label));
            handle_branch_target(label, branch);
            op_stack.start_unreachable();
         } break;
         case opcodes::block: {
            uint32_t expected_result = *code++;
            if (detail::get_allow_zero_blocktype(_options) && expected_result == 0)
               expected_result = types::pseudo;
            detail::check<exceptions::parse>((expected_result == types::i32 || expected_result == types::i64 ||
                                              expected_result == types::f32 || expected_result == types::f64 ||
                                              expected_result == types::pseudo),
                                             "Invalid type code in block");
            pc_stack.push_back({op_stack.depth(), expected_result, expected_result, false, std::vector<branch_t>{}});
            code_writer.emit_block();
            op_stack.push_scope();
            _control_depth_checker.on_control(_options);
            _compatibility_nested_checker.on_control(_options);
         } break;
         case opcodes::loop: {
            uint32_t expected_result = *code++;
            if (detail::get_allow_zero_blocktype(_options) && expected_result == 0)
               expected_result = types::pseudo;
            detail::check<exceptions::parse>((expected_result == types::i32 || expected_result == types::i64 ||
                                              expected_result == types::f32 || expected_result == types::f64 ||
                                              expected_result == types::pseudo),
                                             "Invalid type code in loop");
            auto pos = code_writer.emit_loop();
            pc_stack.push_back({op_stack.depth(), expected_result, types::pseudo, false, pos});
            op_stack.push_scope();
            _control_depth_checker.on_control(_options);
            _compatibility_nested_checker.on_control(_options);
         } break;
         case opcodes::if_: {
            check_in_bounds();
            uint32_t expected_result = *code++;
            if (detail::get_allow_zero_blocktype(_options) && expected_result == 0)
               expected_result = types::pseudo;
            detail::check<exceptions::parse>((expected_result == types::i32 || expected_result == types::i64 ||
                                              expected_result == types::f32 || expected_result == types::f64 ||
                                              expected_result == types::pseudo),
                                             "Invalid type code in if");
            auto branch = code_writer.emit_if();
            op_stack.pop(types::i32);
            pc_stack.push_back({op_stack.depth(), expected_result, expected_result, true, std::vector{branch}});
            op_stack.push_scope();
            _control_depth_checker.on_control(_options);
            _compatibility_nested_checker.on_control(_options);
         } break;
         case opcodes::else_: {
            check_in_bounds();
            auto& old_index = pc_stack.back();
            detail::check<exceptions::parse>((old_index.is_if), "else outside if");
            auto& relocations = std::get<std::vector<branch_t>>(old_index.relocations);
            // reset the operand stack to the same state as the if
            op_stack.pop(old_index.expected_result);
            op_stack.pop_scope();
            op_stack.push_scope();
            // Overwrite the branch from the `if` with the `else`.
            // We're left with a normal relocation list where everything
            // branches to the corresponding `end`
            relocations[0] = code_writer.emit_else(relocations[0]);
            old_index.is_if = false;
            _compatibility_nested_checker.on_control(_options);
            break;
         }
         case opcodes::br: {
            check_in_bounds();
            uint32_t label = parse_varuint32(code);
            auto branch = code_writer.emit_br(compute_depth_change(label));
            handle_branch_target(label, branch);
            op_stack.start_unreachable();
         } break;
         case opcodes::br_if: {
            check_in_bounds();
            uint32_t label = parse_varuint32(code);
            op_stack.pop(types::i32);
            auto branch = code_writer.emit_br_if(compute_depth_change(label));
            handle_branch_target(label, branch);
         } break;
         case opcodes::br_table: {
            check_in_bounds();
            size_t table_size = parse_varuint32(code);
            detail::check<exceptions::parse>((table_size <= detail::get_max_br_table_elements(_options)),
                                             "Too many labels in br_table");
            uint8_t result_type = 0;
            op_stack.pop(types::i32);
            auto handler = code_writer.emit_br_table(table_size);
            for (size_t i = 0; i < table_size; i++) {
               uint32_t label = parse_varuint32(code);
               auto branch = handler.emit_case(compute_depth_change(label));
               handle_branch_target(label, branch);
               uint8_t one_result = pc_stack[pc_stack.size() - label - 1].label_result;
               if (i == 0) {
                  result_type = one_result;
               } else {
                  detail::check<exceptions::parse>((result_type == one_result),
                                                   "br_table labels must have the same type");
               }
            }
            uint32_t label = parse_varuint32(code);
            auto branch = handler.emit_default(compute_depth_change(label));
            handle_branch_target(label, branch);
            detail::check<exceptions::parse>(
                (table_size == 0 || result_type == pc_stack[pc_stack.size() - label - 1].label_result),
                "br_table labels must have the same type");
            op_stack.start_unreachable();
         } break;
         case opcodes::call: {
            check_in_bounds();
            uint32_t funcnum = parse_varuint32(code);
            const func_type& ft = _mod->get_function_type(funcnum);
            for (uint32_t i = 0; i < ft.param_types.size(); ++i)
               op_stack.pop(ft.param_types[ft.param_types.size() - i - 1]);
            detail::check<exceptions::parse>((ft.return_count <= 1), "unsupported");
            if (ft.return_count)
               op_stack.push(ft.return_type);
            code_writer.emit_call(ft, funcnum);
         } break;
         case opcodes::call_indirect: {
            check_in_bounds();
            uint32_t functypeidx = parse_varuint32(code);
            const func_type& ft = _mod->types.at(functypeidx);
            detail::check<exceptions::parse>((_mod->tables.size() > 0), "call_indirect requires a table");
            op_stack.pop(types::i32);
            for (uint32_t i = 0; i < ft.param_types.size(); ++i)
               op_stack.pop(ft.param_types[ft.param_types.size() - i - 1]);
            detail::check<exceptions::parse>((ft.return_count <= 1), "unsupported");
            if (ft.return_count)
               op_stack.push(ft.return_type);
            code_writer.emit_call_indirect(ft, functypeidx);
            detail::check<exceptions::parse>((*code == 0), "call_indirect must end with 0x00.");
            code++; // 0x00
            break;
         }
         case opcodes::drop:
            check_in_bounds();
            code_writer.emit_drop();
            op_stack.pop();
            break;
         case opcodes::select: {
            check_in_bounds();
            code_writer.emit_select();
            op_stack.pop(types::i32);
            uint8_t t0 = op_stack.pop();
            uint8_t t1 = op_stack.pop();
            detail::check<exceptions::parse>((t0 == t1 || t0 == any_type || t1 == any_type),
                                             "incorrect types for select");
            op_stack.push(t0 != any_type ? t0 : t1);
         } break;
         case opcodes::get_local: {
            uint32_t local_idx = parse_varuint32(code);
            op_stack.push(local_types[local_idx]);
            code_writer.emit_get_local(local_idx);
         } break;
         case opcodes::set_local: {
            check_in_bounds();
            uint32_t local_idx = parse_varuint32(code);
            op_stack.pop(local_types[local_idx]);
            code_writer.emit_set_local(local_idx);
         } break;
         case opcodes::tee_local: {
            check_in_bounds();
            uint32_t local_idx = parse_varuint32(code);
            op_stack.top(local_types[local_idx]);
            code_writer.emit_tee_local(local_idx);
         } break;
         case opcodes::get_global: {
            uint32_t global_idx = parse_varuint32(code);
            op_stack.push(_mod->globals.at(global_idx).type.content_type);
            code_writer.emit_get_global(global_idx);
         } break;
         case opcodes::set_global: {
            check_in_bounds();
            uint32_t global_idx = parse_varuint32(code);
            detail::check<exceptions::parse>((_mod->globals.at(global_idx).type.mutability), "cannot set const global");
            op_stack.pop(_mod->globals.at(global_idx).type.content_type);
            code_writer.emit_set_global(global_idx);
         } break;
#define LOAD_OP(op_name, max_align, type)                                                                              \
   case opcodes::op_name: {                                                                                            \
      check_in_bounds();                                                                                               \
      detail::check<exceptions::parse>((_mod->memories.size() > 0), "load requires memory");                           \
      uint32_t alignment = parse_varuint32(code);                                                                      \
      uint32_t offset = parse_varuint32(code);                                                                         \
      detail::check<exceptions::parse>((alignment <= uint32_t(max_align)), "alignment cannot be greater than size.");  \
      detail::check<exceptions::parse>((offset <= detail::get_max_memory_offset(_options)), "load offset too large."); \
      op_stack.pop(types::i32);                                                                                        \
      op_stack.push(types::type);                                                                                      \
      code_writer.emit_##op_name(alignment, offset);                                                                   \
   } break;

            LOAD_OP(i32_load, 2, i32)
            LOAD_OP(i64_load, 3, i64)
            LOAD_OP(f32_load, 2, f32)
            LOAD_OP(f64_load, 3, f64)
            LOAD_OP(i32_load8_s, 0, i32)
            LOAD_OP(i32_load16_s, 1, i32)
            LOAD_OP(i32_load8_u, 0, i32)
            LOAD_OP(i32_load16_u, 1, i32)
            LOAD_OP(i64_load8_s, 0, i64)
            LOAD_OP(i64_load16_s, 1, i64)
            LOAD_OP(i64_load32_s, 2, i64)
            LOAD_OP(i64_load8_u, 0, i64)
            LOAD_OP(i64_load16_u, 1, i64)
            LOAD_OP(i64_load32_u, 2, i64)

#undef LOAD_OP

#define STORE_OP(op_name, max_align, type)                                                                             \
   case opcodes::op_name: {                                                                                            \
      check_in_bounds();                                                                                               \
      detail::check<exceptions::parse>((_mod->memories.size() > 0), "store requires memory");                          \
      uint32_t alignment = parse_varuint32(code);                                                                      \
      uint32_t offset = parse_varuint32(code);                                                                         \
      detail::check<exceptions::parse>((alignment <= uint32_t(max_align)), "alignment cannot be greater than size.");  \
      detail::check<exceptions::parse>((offset <= detail::get_max_memory_offset(_options)),                            \
                                       "store offset too large.");                                                     \
      op_stack.pop(types::type);                                                                                       \
      op_stack.pop(types::i32);                                                                                        \
      code_writer.emit_##op_name(alignment, offset);                                                                   \
   } break;

            STORE_OP(i32_store, 2, i32)
            STORE_OP(i64_store, 3, i64)
            STORE_OP(f32_store, 2, f32)
            STORE_OP(f64_store, 3, f64)
            STORE_OP(i32_store8, 0, i32)
            STORE_OP(i32_store16, 1, i32)
            STORE_OP(i64_store8, 0, i64)
            STORE_OP(i64_store16, 1, i64)
            STORE_OP(i64_store32, 2, i64)

#undef STORE_OP

         case opcodes::current_memory:
            detail::check<exceptions::parse>((_mod->memories.size() != 0), "memory.size requires memory");
            op_stack.push(types::i32);
            detail::check<exceptions::parse>((*code == 0), "memory.size must end with 0x00");
            code++;
            code_writer.emit_current_memory();
            break;
         case opcodes::grow_memory:
            check_in_bounds();
            detail::check<exceptions::parse>((_mod->memories.size() != 0), "memory.grow requires memory");
            op_stack.pop(types::i32);
            op_stack.push(types::i32);
            detail::check<exceptions::parse>((*code == 0), "memory.grow must end with 0x00");
            code++;
            code_writer.emit_grow_memory();
            break;
         case opcodes::i32_const:
            code_writer.emit_i32_const(parse_varint32(code));
            op_stack.push(types::i32);
            break;
         case opcodes::i64_const:
            code_writer.emit_i64_const(parse_varint64(code));
            op_stack.push(types::i64);
            break;
         case opcodes::f32_const: {
            code_writer.emit_f32_const(parse_raw<float>(code));
            op_stack.push(types::f32);
         } break;
         case opcodes::f64_const: {
            code_writer.emit_f64_const(parse_raw<double>(code));
            op_stack.push(types::f64);
         } break;

#define UNOP(opname)                                                                                                   \
   case opcodes::opname:                                                                                               \
      check_in_bounds();                                                                                               \
      code_writer.emit_##opname();                                                                                     \
      op_stack.pop(types::A);                                                                                          \
      op_stack.push(types::R);                                                                                         \
      break;
#define BINOP(opname)                                                                                                  \
   case opcodes::opname:                                                                                               \
      check_in_bounds();                                                                                               \
      code_writer.emit_##opname();                                                                                     \
      op_stack.pop(types::A);                                                                                          \
      op_stack.pop(types::A);                                                                                          \
      op_stack.push(types::R);                                                                                         \
      break;
#define CASTOP(dst, opname, src)                                                                                       \
   case opcodes::dst##_##opname##_##src:                                                                               \
      check_in_bounds();                                                                                               \
      code_writer.emit_##dst##_##opname##_##src();                                                                     \
      op_stack.pop(types::src);                                                                                        \
      op_stack.push(types::dst);                                                                                       \
      break;

#define R i32
#define A i32
            UNOP(i32_eqz)
            BINOP(i32_eq)
            BINOP(i32_ne)
            BINOP(i32_lt_s)
            BINOP(i32_lt_u)
            BINOP(i32_gt_s)
            BINOP(i32_gt_u)
            BINOP(i32_le_s)
            BINOP(i32_le_u)
            BINOP(i32_ge_s)
            BINOP(i32_ge_u)
#undef A
#define A i64
            UNOP(i64_eqz)
            BINOP(i64_eq)
            BINOP(i64_ne)
            BINOP(i64_lt_s)
            BINOP(i64_lt_u)
            BINOP(i64_gt_s)
            BINOP(i64_gt_u)
            BINOP(i64_le_s)
            BINOP(i64_le_u)
            BINOP(i64_ge_s)
            BINOP(i64_ge_u)
#undef A
#define A f32
            BINOP(f32_eq)
            BINOP(f32_ne)
            BINOP(f32_lt)
            BINOP(f32_gt)
            BINOP(f32_le)
            BINOP(f32_ge)
#undef A
#define A f64
            BINOP(f64_eq)
            BINOP(f64_ne)
            BINOP(f64_lt)
            BINOP(f64_gt)
            BINOP(f64_le)
            BINOP(f64_ge)
#undef A
#undef R
#define R A
#define A i32
            UNOP(i32_clz)
            UNOP(i32_ctz)
            UNOP(i32_popcnt)
            BINOP(i32_add)
            BINOP(i32_sub)
            BINOP(i32_mul)
            BINOP(i32_div_s)
            BINOP(i32_div_u)
            BINOP(i32_rem_s)
            BINOP(i32_rem_u)
            BINOP(i32_and)
            BINOP(i32_or)
            BINOP(i32_xor)
            BINOP(i32_shl)
            BINOP(i32_shr_s)
            BINOP(i32_shr_u)
            BINOP(i32_rotl)
            BINOP(i32_rotr)
#undef A
#define A i64
            UNOP(i64_clz)
            UNOP(i64_ctz)
            UNOP(i64_popcnt)
            BINOP(i64_add)
            BINOP(i64_sub)
            BINOP(i64_mul)
            BINOP(i64_div_s)
            BINOP(i64_div_u)
            BINOP(i64_rem_s)
            BINOP(i64_rem_u)
            BINOP(i64_and)
            BINOP(i64_or)
            BINOP(i64_xor)
            BINOP(i64_shl)
            BINOP(i64_shr_s)
            BINOP(i64_shr_u)
            BINOP(i64_rotl)
            BINOP(i64_rotr)
#undef A
#define A f32
            UNOP(f32_abs)
            UNOP(f32_neg)
            UNOP(f32_ceil)
            UNOP(f32_floor)
            UNOP(f32_trunc)
            UNOP(f32_nearest)
            UNOP(f32_sqrt)
            BINOP(f32_add)
            BINOP(f32_sub)
            BINOP(f32_mul)
            BINOP(f32_div)
            BINOP(f32_min)
            BINOP(f32_max)
            BINOP(f32_copysign)
#undef A
#define A f64
            UNOP(f64_abs)
            UNOP(f64_neg)
            UNOP(f64_ceil)
            UNOP(f64_floor)
            UNOP(f64_trunc)
            UNOP(f64_nearest)
            UNOP(f64_sqrt)
            BINOP(f64_add)
            BINOP(f64_sub)
            BINOP(f64_mul)
            BINOP(f64_div)
            BINOP(f64_min)
            BINOP(f64_max)
            BINOP(f64_copysign)
#undef A
#undef R

            CASTOP(i32, wrap, i64)
            CASTOP(i32, trunc_s, f32)
            CASTOP(i32, trunc_u, f32)
            CASTOP(i32, trunc_s, f64)
            CASTOP(i32, trunc_u, f64)
            CASTOP(i64, extend_s, i32)
            CASTOP(i64, extend_u, i32)
            CASTOP(i64, trunc_s, f32)
            CASTOP(i64, trunc_u, f32)
            CASTOP(i64, trunc_s, f64)
            CASTOP(i64, trunc_u, f64)
            CASTOP(f32, convert_s, i32)
            CASTOP(f32, convert_u, i32)
            CASTOP(f32, convert_s, i64)
            CASTOP(f32, convert_u, i64)
            CASTOP(f32, demote, f64)
            CASTOP(f64, convert_s, i32)
            CASTOP(f64, convert_u, i32)
            CASTOP(f64, convert_s, i64)
            CASTOP(f64, convert_u, i64)
            CASTOP(f64, promote, f32)
            CASTOP(i32, reinterpret, f32)
            CASTOP(i64, reinterpret, f64)
            CASTOP(f32, reinterpret, i32)
            CASTOP(f64, reinterpret, i64)

#undef CASTOP
#undef UNOP
#undef BINOP
         default:
            detail::fail<exceptions::parse>("Illegal instruction");
         }
      }
      detail::check<exceptions::parse>((pc_stack.empty()), "function body too long");
      _mod->maximum_stack = std::max(_mod->maximum_stack, static_cast<uint64_t>(op_stack.maximum_operand_depth) +
                                                              local_types.locals_count());
   }

   void parse_data_segment(wasm_code_ptr& code, data_segment& ds) {
      detail::check<exceptions::parse>((_mod->memories.size() != 0), "data requires memory");
      ds.index = parse_varuint32(code);
      detail::check<exceptions::parse>((ds.index == 0), "only memory index of 0 is supported");
      parse_init_expr(code, ds.offset, types::i32);
      auto len = parse_varuint32(code);
      detail::check<exceptions::parse>((len <= detail::get_max_data_segment_bytes(_options)),
                                       "data segment too large.");
      detail::check<exceptions::parse>((static_cast<uint64_t>(static_cast<uint32_t>(ds.offset.value.i32)) + len <=
                                        detail::get_max_linear_memory_init(_options)),
                                       "out-of-bounds data section");
      auto guard = code.scoped_shrink_bounds(len);
      ds.data = decltype(ds.data){_allocator, len};
      ds.data.copy(code.raw(), len);
      code += len;
   }

   template <typename Elem, typename ParseFunc>
   inline void parse_section_impl(wasm_code_ptr& code, vec<Elem>& elems, std::uint32_t max_elements,
                                  ParseFunc&& elem_parse) {
      auto count = parse_varuint32(code);
      detail::check<exceptions::parse>((count <= max_elements), "number of section elements exceeded limit");
      elems = vec<Elem>{_allocator, count};
      for (size_t i = 0; i < count; i++) {
         elem_parse(code, elems.at(i), i);
      }
   }

   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::type_section, func_type>>& elems) {
      parse_section_impl(code, elems, detail::get_max_type_section_elements(_options),
                         [&](wasm_code_ptr& code, func_type& ft, std::size_t /*idx*/) { parse_func_type(code, ft); });
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::import_section, import_entry>>& elems) {
      parse_section_impl(
          code, elems, detail::get_max_import_section_elements(_options),
          [&](wasm_code_ptr& code, import_entry& ie, std::size_t /*idx*/) { parse_import_entry(code, ie); });
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::function_section, uint32_t>>& elems) {
      parse_section_impl(
          code, elems, detail::get_max_function_section_elements(_options),
          [&](wasm_code_ptr& code, uint32_t& elem, std::size_t /*idx*/) { elem = parse_varuint32(code); });
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::table_section, table_type>>& elems) {
      parse_section_impl(code, elems, 1,
                         [&](wasm_code_ptr& code, table_type& tt, std::size_t /*idx*/) { parse_table_type(code, tt); });
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::memory_section, memory_type>>& elems) {
      parse_section_impl(code, elems, 1, [&](wasm_code_ptr& code, memory_type& mt, std::size_t idx) {
         detail::check<exceptions::parse>((idx == 0), "only one memory is permitted");
         parse_memory_type(code, mt);
      });
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::global_section, global_variable>>& elems) {
      parse_section_impl(
          code, elems, detail::get_max_global_section_elements(_options),
          [&](wasm_code_ptr& code, global_variable& gv, std::size_t /*idx*/) { parse_global_variable(code, gv); });
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::export_section, export_entry>>& elems) {
      parse_section_impl(
          code, elems, detail::get_max_export_section_elements(_options),
          [&](wasm_code_ptr& code, export_entry& ee, std::size_t /*idx*/) { parse_export_entry(code, ee); });
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             typename std::enable_if_t<id == section_id::start_section, uint32_t>& start) {
      start = parse_varuint32(code);
      const func_type& ft = _mod->get_function_type(start);
      detail::check<exceptions::parse>((ft.return_count == 0 && ft.param_types.size() == 0), "wrong type for start");
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::element_section, elem_segment>>& elems) {
      parse_section_impl(
          code, elems, detail::get_max_element_section_elements(_options),
          [&](wasm_code_ptr& code, elem_segment& es, std::size_t /*idx*/) { parse_elem_segment(code, es); });
   }
   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::code_section, function_body>>& elems) {
      const void* code_start = code.raw() - code.offset();
      parse_section_impl(
          code, elems, detail::get_max_function_section_elements(_options),
          [&](wasm_code_ptr& code, function_body& fb, std::size_t idx) { parse_function_body(code, fb, idx); });
      detail::check<exceptions::parse>((elems.size() == _mod->functions.size()),
                                       "code section must have the same size as the function section");

      write_code_out(_allocator, code, code_start);
   }

   void write_code_out(growable_allocator& allocator, wasm_code_ptr& code, const void* code_start) {
      Writer code_writer(allocator, code.bounds() - code.offset(), *_mod);
      imap.on_code_start(code_writer.get_base_addr(), code_start);
      for (size_t i = 0; i < _function_bodies.size(); i++) {
         function_body& fb = _mod->code[i];
         func_type& ft = _mod->types.at(_mod->functions.at(i));
         local_types_t local_types(ft, fb.locals);
         imap.on_function_start(code_writer.get_addr(), _function_bodies[i].first.raw());
         code_writer.emit_prologue(ft, fb.locals, i);
         parse_function_body_code(_function_bodies[i].first, fb.size, _function_bodies[i].second, code_writer, ft,
                                  local_types);
         code_writer.emit_epilogue(ft, fb.locals, i);
         code_writer.finalize(fb);
      }
      imap.on_code_end(code_writer.get_addr(), code.raw());
   }

   template <uint8_t id>
   inline void parse_section(wasm_code_ptr& code,
                             vec<typename std::enable_if_t<id == section_id::data_section, data_segment>>& elems) {
      parse_section_impl(
          code, elems, detail::get_max_data_section_elements(_options),
          [&](wasm_code_ptr& code, data_segment& ds, std::size_t /*idx*/) { parse_data_segment(code, ds); });
   }

   template <size_t N> varint<N> parse_varint(const wasm_code& code, size_t index) {
      varint<N> result(0);
      result.set(code, index);
      return result;
   }

   template <size_t N> varuint<N> parse_varuint(const wasm_code& code, size_t index) {
      varuint<N> result(0);
      result.set(code, index);
      return result;
   }

   void on_mutable_global(uint8_t type) {
      _globals_checker.on_mutable_global(_options, type);
   }

   void validate_exports() const {
      std::vector<const guarded_vector<uint8_t>*> export_names;
      export_names.reserve(_mod->exports.size());
      for (uint32_t i = 0; i < _mod->exports.size(); ++i) {
         export_names.push_back(&_mod->exports[i].field_str);
      }
      std::sort(export_names.begin(), export_names.end(), [](auto* lhs, auto* rhs) {
         return std::lexicographical_compare(lhs->raw(), lhs->raw() + lhs->size(), rhs->raw(),
                                             rhs->raw() + rhs->size());
      });
      auto it = std::adjacent_find(export_names.begin(), export_names.end(), [](auto* lhs, auto* rhs) {
         return lhs->size() == rhs->size() && std::equal(lhs->raw(), lhs->raw() + lhs->size(), rhs->raw());
      });
      detail::check<exceptions::parse>((it == export_names.end()), "duplicate export name");
   }

 private:
   growable_allocator& _allocator;
   Options _options;
   module* _mod; // non-owning weak pointer
   int64_t _current_function_index = -1;
   uint64_t _maximum_function_stack_usage = 0; // non-parameter locals + stack
   std::vector<std::pair<wasm_code_ptr, detail::max_func_local_bytes_stack_checker<Options>>> _function_bodies;
   detail::max_mutable_globals_checker<Options> _globals_checker;
   detail::max_control_depth_checker<Options> _control_depth_checker;
   detail::max_legacy_nested_structures_checker<Options> _compatibility_nested_checker;
   typename DebugInfo::builder imap;
};
} // namespace forge::vm::wasm::interpret
