---
name: create-db
description: Create, change, format, or review host-side forge.db.object models and index declarations, with explicit BitShares-style template blocks. Extends create-library; does not govern Contract SDK guest multi_index tables.
---

# Skill: DB Object Models And Indexes

Apply [create-library](../create-library/SKILL.md) first for file ownership,
module/source pairing and declaration-before-metadata placement. This skill
adds the host-side `forge.db.object` declaration style; Contract SDK guest
`multi_index` tables use [create-contract-library](../create-contract-library/SKILL.md).

Read the current [DB Object README](../../libraries/db/object/README.md) for
the descriptor, registration, transaction and ranked-schema contracts. Use
existing `object_index`, `indexed_by`, `primary_unique`, `ordered_*`, `ranked_*`,
`member`, `const_mem_fun` and `composite_key` templates; do not create wrappers
or a second indexing mechanism.

## Declaration Layout

- Indent by 3 spaces and keep lines within 120 columns.
- Declare the model before its index descriptor. Put the object type argument
  and `indexed_by<...>` on separate lines beneath `object_index<`.
- Keep a simple index on one line when it fits, including its short leaf
  extractor. Split a complex `composite_*` index into its own block. Its tag
  may share the `ordered_*<` or `ranked_*<` line when that line fits; start the
  composite block on the next line and give each key component its own line.
- Break a long variadic `member<...>` into one member-pointer step per line.
  Preserve the extraction path visibly instead of introducing an alias.
- Close each complex template level on a separate line at its opening
  indentation. A short leaf such as `member<&item::name>>` may keep `>>`;
  never collapse several complex levels into a closing tail such as `>>>>;`.
- Keep existing comparator, `ranked_schema<...>` and other extra arguments
  after the composite block, at the enclosing index's argument indentation.
  Follow the actual Forge template signature; donor comparator APIs do not
  become Forge APIs through this style rule.
- Do not add `typedef`, `using namespace`, namespace aliases or extractor
  aliases to shorten declarations. The natural `using item_index = ...`
  descriptor declaration is expected.
- Put reflection macros, `FORGE_DB_OBJECT(...)` and schema specializations in
  the final metadata block with their required namespace/export placement.

Clang-format 22 with the current repository configuration collapses this
layout. Preserve these template blocks manually after formatting. Do not add
scattered `clang-format off` regions or change the global formatter for DB
declarations.

## Example

Fragment of an owning module that imports `forge.db.object.object` and
`forge.db.object.index`, with the normal standard headers, Boost.Describe and
`forge/db/object/macros.hpp` available:

```cpp
export namespace forge::example {

struct payload_stats {
   std::uint64_t bytes = 0;
};

struct payload {
   payload_stats stats;
};

struct item : forge::db::object::object<item, 1, 7> {
   std::string name;
   std::uint32_t group = 0;
   payload content;

   constexpr std::uint64_t size() const noexcept
   {
      return content.stats.bytes;
   }
};

struct by_id;
struct by_name;
struct by_group_size;
struct by_group_bytes;

using item_index = forge::db::object::object_index<
   item,
   forge::db::object::indexed_by<
      forge::db::object::primary_unique<by_id>,
      forge::db::object::ordered_unique<by_name, forge::db::object::member<&item::name>>,
      forge::db::object::ordered_non_unique<by_group_size,
         forge::db::object::composite_key<
            forge::db::object::member<&item::group>,
            forge::db::object::descending<
               forge::db::object::const_mem_fun<&item::size>
            >
         >
      >,
      forge::db::object::ranked_non_unique<by_group_bytes,
         forge::db::object::composite_key<
            forge::db::object::member<&item::group>,
            forge::db::object::member<
               &item::content,
               &payload::stats,
               &payload_stats::bytes
            >
         >,
         forge::db::object::ranked_schema<1>
      >
   >
>;

BOOST_DESCRIBE_STRUCT(payload_stats, (), (bytes))
BOOST_DESCRIBE_STRUCT(payload, (), (stats))
BOOST_DESCRIBE_STRUCT(item, (forge::db::object::object<item, 1, 7>), (name, group, content))

}

FORGE_DB_OBJECT(forge::example::item_index)
```

## Semantic And Validation Boundary

Formatting preserves descriptor and key order, uniqueness, extractor types
and paths, object IDs, schema versions, rank/sum definitions, comparators and
transaction behavior. For an intentional persisted-schema change, follow the
README's explicit migration/rebuild contract; do not disguise it as formatting.
Models remain backend-neutral declarations; runtime registration and mutations
use the existing store and transaction APIs.

For formatting-only edits, verify C++ token equivalence before and after,
allowing whitespace changes and equivalent `>>`/`> >` template closings, and
inspect the resulting blocks and metadata placement. Use compiler tokens or a
C++ lexer; stripping all whitespace is not sufficient because it can hide
changes inside literals. A full runtime build is not required solely for
whitespace. For semantic changes, run the owning model/index checks and
relevant DB tests.

The formatting donor is BitShares Core at
`7e4595b3b823bf68ede2e3ab8b76b0005c78f72a`:
[account indexes](https://github.com/bitshares/bitshares-core/blob/7e4595b3b823bf68ede2e3ab8b76b0005c78f72a/libraries/chain/include/graphene/chain/account_object.hpp#L375-L390)
and [market indexes](https://github.com/bitshares/bitshares-core/blob/7e4595b3b823bf68ede2e3ab8b76b0005c78f72a/libraries/chain/include/graphene/chain/market_object.hpp#L79-L121).
This adopts declaration readability, not Graphene backend semantics. Broader
mechanism provenance remains in the
[ObjectDB donor baseline](../../docs/donors/forge-objectdb-donor-baseline-v1.md)
and [ranked-index donor note](../../docs/donors/forge-db-object-aggregates-v1.md).
