module;

#include <cstddef>
#include <span>

export module forge.db.authenticated.codec;

export import forge.db.authenticated.types;

export namespace forge::db::authenticated {

[[nodiscard]] bytes encode(const point_proof& value);
[[nodiscard]] bytes encode(const range_proof& value);
[[nodiscard]] bytes encode(const checkpoint_record& value);
[[nodiscard]] std::size_t wire_size(const point_proof& value);
[[nodiscard]] std::size_t wire_size(const range_proof& value);
[[nodiscard]] std::size_t wire_size(const range_proof_node& value);
[[nodiscard]] std::size_t wire_size(const checkpoint_record& value);
void require_wire_budget(std::size_t size, const limits& settings = {});
[[nodiscard]] point_proof decode_point(std::span<const std::byte> value, const limits& settings = {});
[[nodiscard]] range_proof decode_range(std::span<const std::byte> value, const limits& settings = {});
[[nodiscard]] checkpoint_record decode_checkpoint_record(std::span<const std::byte> value,
                                                          const limits& node_bounds,
                                                          const checkpoint_limits& checkpoint_bounds);

} // namespace forge::db::authenticated
