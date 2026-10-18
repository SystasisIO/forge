#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Test-only transport of already encoded values. Defined once by the ordinary
// fixture owner; the owning IR unit declares it in its global module fragment.
// No production diagnostic API, metadata converter, parser or artifact codec.
namespace optimizing_fixture_export {
void write(std::string_view name, std::span<const std::uint8_t> request,
           std::span<const std::uint8_t> artifact);
// One existing ordinary fixture owner supplies encoded canonical test input;
// the private owning IR unit decodes through the SAME public request owner.
std::vector<std::uint8_t> finite_request(std::uint32_t parameters, bool darwin);
void write_diagnostic(std::string_view name, std::string_view extension,
                      std::span<const std::uint8_t> bytes);
}
