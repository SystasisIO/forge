#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>

namespace forge::tests::p2p {

class pubsub_write_allocation_fixture {
 public:
   enum class factory { chunk, frame_chunk };
   enum class allocator { none, scalar_new, aligned_alloc };

   struct result {
      bool ordinary_thread = false;
      allocator rejected_by = allocator::none;
      std::exception_ptr construction_error;
      std::size_t factories_after_failure = 0;
      std::size_t bodies_after_failure = 0;
      std::size_t factories_before_recovery_execution = 0;
      std::size_t bodies_before_recovery_execution = 0;
      std::size_t factories_after_recovery = 0;
      std::size_t bodies_after_recovery = 0;
      bool payload_preserved = false;
      bool selected_factory_preserved = false;
   };

   struct trace_result {
      allocator rejected_by = allocator::none;
      std::exception_ptr native_error;
      bool authenticated = false;
      bool heap_peer_id = false;
      bool snapshot_observed = false;
      bool subscription_succeeded = false;
      bool same_native_stream = false;
      bool recovery_delivered = false;
      std::uint64_t write_callbacks = 0;
      std::uint64_t native_subscription_frames = 0;
      std::uint64_t trace_failures = 0;
      std::uint64_t outbound_bytes_after_failure = 0;
      std::uint64_t outbound_bytes_after_recovery = 0;
      std::uint64_t stream_memory_after_failure = 0;
      std::uint64_t stream_memory_after_recovery = 0;
      std::uint64_t messages_published = 0;
      std::uint64_t messages_delivered = 0;
      bool native_failures_unchanged = false;
   };

   class allocation_scope {
    public:
      allocation_scope() noexcept;
      ~allocation_scope();
      allocation_scope(const allocation_scope&) = delete;
      allocation_scope& operator=(const allocation_scope&) = delete;
      [[nodiscard]] allocator rejected_by() const noexcept;
   };

   [[nodiscard]] static result observe(factory selected);
   [[nodiscard]] static trace_result observe_native_trace(std::uint64_t (*outbound_bytes)(const void*));
   static void arm_trace_allocation(trace_result&) noexcept;
   [[nodiscard]] static bool reject_allocation(allocator source) noexcept;

 private:
   class write_model;
   static thread_local bool _armed;
   static thread_local allocator _rejected_by;
   static thread_local trace_result* _trace_target;
};

} // namespace forge::tests::p2p
