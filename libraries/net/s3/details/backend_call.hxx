#pragma once

namespace forge::net::s3::detail {

template <typename Work> decltype(auto) backend_call(Work&& work, const std::atomic<bool>* mutation_started = nullptr) {
   try {
      return std::invoke(std::forward<Work>(work));
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (...) {
      if (mutation_started && mutation_started->load()) {
         FORGE_THROW_EXCEPTION(exceptions::unknown_outcome,
                               "S3 backend failed after possible mutation; reconcile before retry");
      }
      FORGE_THROW_EXCEPTION(exceptions::service, "S3 backend failed before a confirmed result");
   }
}

} // namespace forge::net::s3::detail
