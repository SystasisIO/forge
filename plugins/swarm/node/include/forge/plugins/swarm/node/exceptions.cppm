module;

#include <forge/exceptions/macros.hpp>
#include <cstdint>
#include <exception>
#include <source_location>
#include <string>

export module forge.plugins.swarm.node.exceptions;

export import forge.exceptions;

export namespace forge::plugins::swarm::node {

class exceptions {
 public:
   enum class code : std::uint16_t {
      invalid_config = 1, not_running = 2, metadata_unavailable = 3,
      canceled = 4, timeout = 5, operation_failed = 6,
   };
   struct failures { std::exception_ptr primary; std::exception_ptr cleanup; };
   class operation_failed;
   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using not_running = forge::exceptions::coded_exception<code, code::not_running>;
   using metadata_unavailable = forge::exceptions::coded_exception<code, code::metadata_unavailable>;
   using canceled = forge::exceptions::coded_exception<code, code::canceled>;
   using timeout = forge::exceptions::coded_exception<code, code::timeout>;
};

class exceptions::operation_failed final : public forge::exceptions::coded_exception<code, code::operation_failed> {
 public:
   operation_failed(failures, std::string, forge::exceptions::fields,
      std::source_location = std::source_location::current());
   [[nodiscard]] const failures& causes() const noexcept;
 private:
   failures _causes;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.swarm.node")

} // namespace forge::plugins::swarm::node
