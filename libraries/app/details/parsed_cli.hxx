#pragma once

namespace forge::app::detail {

struct parsed_cli {
   forge::config::core::document document;
   std::vector<std::string> filtered_args;
   std::vector<forge::schema::diagnostic> diagnostics;
   bool help = false;
   bool check_config = false;
   bool print_effective_config = false;
   bool configure = false;
   bool config_explicit = false;
   bool dotenv_explicit = false;

   [[nodiscard]] constexpr bool ok() const noexcept {
      return diagnostics.empty();
   }
};

} // namespace forge::app::detail
