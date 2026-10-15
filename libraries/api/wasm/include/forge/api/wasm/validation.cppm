module;

#include <cstdint>
#include <span>
#include <string_view>

export module forge.api.wasm.validation;

export import forge.api.wasm.capability;
export import forge.api.wasm.context;
export import forge.api.wasm.event;
export import forge.api.wasm.exceptions;
export import forge.api.wasm.http_command;
export import forge.api.wasm.http_stream;
export import forge.api.wasm.limits;
export import forge.api.wasm.step;

export namespace forge::api::wasm {

void validate_type_name(std::string_view value, const limits& bounds = {});
void validate(const payload& value, const limits& bounds = {});
void validate(const payload& value, std::string_view expected_type, const limits& bounds = {});
void validate(const continuation& value, const limits& bounds = {});
void validate(const continuation& value, std::uint32_t expected_version, std::string_view expected_type,
              const limits& bounds = {});
void validate(const effect& value, const limits& bounds = {});
void validate(const awaiting& value, const limits& bounds = {});
void validate(const completed& value, const limits& bounds = {});
void validate(const step& value, const limits& bounds = {});
void validate(const event& value, const limits& bounds = {});
void validate(const subject_view& value, const limits& bounds = {});
void validate(const context& value, const limits& bounds = {});
void validate(const http_command& value, const limits& bounds = {});
void validate(const http_response& value, const limits& bounds = {});
void validate(const http_stream_open& value, const limits& bounds = {});
void validate(const http_stream_read& value, const limits& bounds = {});
void validate(const http_stream_event& value, const limits& bounds = {});
void validate(const http_stream_event& value, std::string_view expected_item_type, const limits& bounds = {});
void validate(const core::error_payload& value, const limits& bounds = {});

void validate(const capability& allowed, const limits& bounds = {});
void validate(const effect& value, const capability& allowed, const limits& bounds = {});
void validate(const effect& value, std::span<const capability> allowed, const limits& bounds = {});
void validate(const step& value, std::span<const capability> allowed, const limits& bounds = {});
void validate(const event& value, const effect& pending, const capability& allowed, const limits& bounds = {});
void validate(const event& value, const effect& pending, std::span<const capability> allowed,
              const limits& bounds = {});
void validate(const http_command& value, const capability::http_policy& allowed, const limits& bounds = {});
void validate(const http_response& value, const capability::http_policy& allowed, const limits& bounds = {});

} // namespace forge::api::wasm
