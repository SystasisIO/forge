#pragma once

// Included after the owning compiler_session module declaration. Completion
// notification uses the existing Forge Asio notification owner, not another
// completion channel/timer, scheduler or generic notification framework.
namespace forge::vm::wasm::optimizing {

struct compiler_session::ticket::state {
   struct outcome {
      std::optional<completion> value;
      std::exception_ptr error;
   };
   state();
   ~state();
   bool cancel() noexcept;
   void complete(completion) noexcept;
   void fail(std::exception_ptr) noexcept;

   std::mutex mutex;
   bool completed = false;
   bool cancelled = false;
   outcome result;
   asio::notification changed;
};

} // namespace forge::vm::wasm::optimizing
