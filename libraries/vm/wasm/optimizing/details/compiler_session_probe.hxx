#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace forge::vm::wasm::optimizing::detail {

// Private serial-subprocess qualification only. No callback, session control,
// module pin or allocation-owner handle crosses these two fixed boundaries.
class compiler_session_probe {
 public:
   enum class point { reservations_held, prepared_before_return };
   struct snapshot {
      std::uint64_t session;
      std::uint64_t job;
      std::uint64_t resident_bytes;
      std::uint64_t workspace_bytes;
      std::uint64_t configuration_bytes;
      std::uint64_t token_bytes;
      bool prepared;
   };
   struct observation {
      std::array<snapshot, 2> snapshots{};
      std::size_t arrived = 0;
      std::size_t paused = 0;
      bool released = false;
      bool timed_out = false;
      bool overflow = false;
   };

   compiler_session_probe(point, std::chrono::steady_clock::time_point deadline);
   ~compiler_session_probe();
   compiler_session_probe(const compiler_session_probe&) = delete;
   compiler_session_probe& operator=(const compiler_session_probe&) = delete;
   // Bounded wait on the test controller, never on an Asio runtime thread.
   observation wait(std::size_t expected);
   // Detach and open before session shutdown, including every failed assertion.
   // An already-entered worker independently pins only the observer state.
   void close() noexcept;

   static void reservations_held(snapshot) noexcept;
   static void prepared_before_return(snapshot) noexcept;

 private:
   struct state;
   static void pause(point, snapshot) noexcept;
   static std::mutex _mutex;
   static std::shared_ptr<state> _active;
   std::shared_ptr<state> _state;
};

} // namespace forge::vm::wasm::optimizing::detail
