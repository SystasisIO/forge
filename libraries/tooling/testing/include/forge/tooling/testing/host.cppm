module;

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

export module forge.tooling.testing.host;

export import forge.tooling.testing.exceptions;
export import forge.tooling.testing.schema;
export import forge.tooling.testing.state;

#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
import forge.expression.program;
#endif

export namespace forge::tooling::testing {

struct execution_limits {
   std::chrono::milliseconds timeout{std::chrono::seconds{5}};
};

struct invocation_result {
   std::vector<std::uint8_t> return_value;
   std::optional<std::int32_t> exit_code;
};

struct invocation_options {
   std::optional<std::chrono::steady_clock::time_point> deadline;
   std::stop_token cancel;
};

class host final {
 public:
   explicit host(execution_limits limits = {});
#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
   host(forge::expression::runtime& expressions, execution_limits limits = {});
#endif
   ~host();

   host(const host&) = delete;
   host& operator=(const host&) = delete;
   host(host&&) = delete;
   host& operator=(host&&) = delete;

   invocation_result invoke(std::span<const std::uint8_t> code, std::uint64_t receiver, std::uint64_t first_receiver,
                            std::uint64_t action, std::vector<std::uint8_t> data = {});
   invocation_result invoke(std::span<const std::uint8_t> code, std::uint64_t receiver, std::uint64_t first_receiver,
                            std::uint64_t action, std::vector<std::uint8_t> data, invocation_options options);

   // One database/oracle transaction across explicitly driven real-WASM invocations.
   // This does not route inline actions, model authorities, or emulate a chain.
   void transact(const std::function<void()>& body);

   void configure(oracle_state state);
   [[nodiscard]] oracle_state state() const;
   void register_contract(std::uint64_t account, std::vector<std::uint8_t> code);

   [[nodiscard]] std::optional<table> find_table(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name);
   [[nodiscard]] std::optional<key_value> find_primary(std::uint64_t code, std::uint64_t scope,
                                                       std::uint64_t table_name, std::uint64_t primary);
   [[nodiscard]] std::optional<index64> find_index64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                                     std::uint64_t primary);
   [[nodiscard]] std::optional<index128> find_index128(std::uint64_t code, std::uint64_t scope,
                                                       std::uint64_t table_name, std::uint64_t primary);
   [[nodiscard]] std::optional<index256> find_index256(std::uint64_t code, std::uint64_t scope,
                                                       std::uint64_t table_name, std::uint64_t primary);
   [[nodiscard]] std::optional<index_double> find_index_double(std::uint64_t code, std::uint64_t scope,
                                                               std::uint64_t table_name, std::uint64_t primary);
   [[nodiscard]] std::optional<index_long_double>
   find_index_long_double(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name, std::uint64_t primary);
   [[nodiscard]] std::vector<std::uint8_t> snapshot() const;

 private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace forge::tooling::testing
