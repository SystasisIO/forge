# forge_core

`forge_core` — самый нижний слой FORGE: маленькие value/helpers, которые можно
использовать почти везде без риска подтянуть serialization, JSON, crypto,
logging or network dependencies.

## When To Use

- Нужны безопасные string/UTF-8 helpers, `uint128`, type names or version metadata.
- Нужно написать библиотеку верхнего уровня и не создать обратную зависимость на
  `variant`, `raw`, `json`, `log` or `crypto`.

## When Not To Use

- Для файловых путей: используйте `std::filesystem::path`.
- Для binary serialization: это `forge_raw`, не `core`.
- Для динамических JSON-like values: это `forge_variant`.
- Для domain-specific helpers consuming applications should own themselves.

## Public Modules

- `forge.core.string` — numeric parsing and escaped string formatting.
- `forge.core.type_name` — diagnostic-friendly type names.
- `forge.core.uint128` — retained 128-bit value support.
- `forge.core.utf8` — UTF-8 validation/cleanup wrappers.
- `forge.core.utility` — small utility primitives such as `yield_function_t`.
- `forge.core.conversion` — primary `forge::core::conversion<Result>` template
  declaration; domain conversion libraries own explicit specializations.
- `forge.core.version`, `forge.core.git_revision` — build/version metadata.

Target: `forge_core`.

Owned dependencies: minimal Boost implementation details only. `forge_core` must
not import or link `forge_raw`, `forge_variant`, `forge_codec_json`, `forge_log`,
any Crypto leaf or network target.

## Examples

### Parse Numbers And Escape Strings

```cpp
import forge.core.string;

auto value = forge::to_uint64("18446744073709551615");
auto printable = forge::escape_str("line\nbreak", 64);
```

### Declare A Domain Conversion

Import `forge.core.conversion` in a domain conversion module and specialize
`forge::core::conversion<Result>` in `namespace forge::core`. The specialization
owns its input types, validation and assembly; Core supplies no default mapping,
virtual interface, registry or serialization. Its target depends on its input
and result libraries, while those libraries do not depend on the conversion.

Unsupported result types remain incomplete. A conversion must not implicitly
read a database, commit state or treat remote input as trusted local state.

## Risks And Anti-Patterns

- Do not use core helpers to hide application policy. If a rule depends on files,
  network, credentials or daemon layout, it belongs above `forge_core`.
- Do not reintroduce global clocks or mock-time state. Tests should pass
  explicit `std::chrono` values.
- Do not add convenience imports from upper libraries. A small dependency in
  `core` becomes a dependency of everything.

## Typical Mistakes

- Do not reintroduce `forge::time_point` aliases. Public time API is `std::chrono`.
- Do not put raw overloads or `to_variant/from_variant` here. They belong to
  `forge_raw` and `forge_variant`.
- Do not hide filesystem policy in `core`; consumers decide their own path rules.

## Tests

`tests/core` covers string escaping and retained low-level behavior. Any new
`core` API must prove it does not depend on upper FORGE domains.
