module;

#include <filesystem>
#include <string>

export module forge.app.application_environment;

export namespace forge::app {

class application_environment {
 public:
   application_environment() = default;
   explicit application_environment(std::filesystem::path data_dir, std::string profile = {});

   [[nodiscard]] const std::filesystem::path& data_dir() const noexcept;
   [[nodiscard]] const std::string& profile() const noexcept;
   [[nodiscard]] std::filesystem::path resolve_data_path(const std::filesystem::path& path) const;

 private:
   std::filesystem::path data_dir_;
   std::string profile_;
};

} // namespace forge::app
