#pragma once

#include <cstdint>
#include <exception>
#include <string_view>

namespace forge::app::detail {

enum class lifecycle_log_level { debug, info, warning, error };

struct lifecycle_log {
   const char* logger = "forge.app.application";
   const char* event = "app.operation.complete";
   const char* operation = "configure";
   lifecycle_log_level level = lifecycle_log_level::debug;
   int phase = -1;
   std::uint64_t generation = 0;
   std::uint64_t epoch = 0;
   bool stop_requested = false;
   std::exception_ptr error;
   const char* canceled_event = "app.operation.canceled";
   std::string_view id;
   int exit_code = 0;
   bool has_exit_code = false;
};

void emit_lifecycle_log(const lifecycle_log& value) noexcept;

template <typename Snapshot>
void log_application_operation(const Snapshot& snapshot, const char* operation, bool beginning,
                               std::exception_ptr error = {}) noexcept {
   const auto name = std::string_view{operation};
   if (!beginning && !error && name == "startup" && snapshot.phase != decltype(snapshot.phase)::started) {
      return;
   }
   const auto retry = beginning && name == "shutdown" && snapshot.failure &&
                      snapshot.failure->operation == decltype(snapshot.failure->operation)::shutdown;
   emit_lifecycle_log({.event = retry ? "app.cleanup.retry" : beginning ? "app.operation.begin"
                               : error ? "app.operation.failed"
                               : name == "startup" ? "app.ready"
                               : name == "shutdown" ? "app.cleanup.complete" : "app.operation.complete",
                       .operation = operation,
                       .level = retry ? lifecycle_log_level::warning
                           : !beginning && (name == "startup" || name == "shutdown")
                           ? lifecycle_log_level::info : lifecycle_log_level::debug,
                       .phase = static_cast<int>(snapshot.phase),
                       .generation = snapshot.generation,
                       .epoch = snapshot.epoch,
                       .stop_requested = snapshot.stop_requested,
                       .error = error});
}

template <typename Snapshot>
void log_application_stop(const Snapshot& snapshot, bool live) noexcept {
   emit_lifecycle_log({.event = "app.stop.requested",
                       .operation = "request_stop",
                       .level = live ? lifecycle_log_level::info : lifecycle_log_level::debug,
                       .phase = static_cast<int>(snapshot.phase),
                       .generation = snapshot.generation,
                       .epoch = snapshot.epoch,
                       .stop_requested = snapshot.stop_requested});
}

} // namespace forge::app::detail
