# Savanna Producer Opportunity Module Boundary

`forge.chain.savanna.producer_opportunity` now owns the existing
`forge::chain::savanna::producer_opportunity` value record and its Describe
metadata. `forge.chain.savanna.finality_witness` re-exports it, preserving the
existing source import contract. No target or package component is added.

The four fields retain their exact canonical C++ types, order, default
initialization and equality. Qualified Chain Protocol names replace the old
equivalent Savanna aliases in the declaration only. Reflection field order and
wire encoding are unchanged; replay, proof validation and finality limits are
not modified.

Consumers that need only the record can import the narrow module without
loading witness replay, checkpoint or admission declarations. Rebuild compiled
module interfaces and dependent archives normally because named-module
attachment changes. Do not reuse old compiled interfaces with the new record.

The Spine console fixture is the qualifying consumer. Source separation alone
does not prove its compilation or the existing Savanna serialization tests.
