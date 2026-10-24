module;

#include <cstdint>
#include <span>

export module forge.expression.artifact_codec;

export import forge.expression.artifact;
import forge.expression.exceptions;

export namespace forge::expression {

// Envelope validation only. program(runtime, artifact) additionally rechecks
// the actual CEL tree, types and references inside the bounded native worker.
void validate(const artifact&);
[[nodiscard]] crypto::digest::sha256 artifact_digest(const artifact&);
[[nodiscard]] raw::bytes encode_artifact(const artifact&);
[[nodiscard]] artifact decode_artifact(std::span<const std::uint8_t>);

} // namespace forge::expression
