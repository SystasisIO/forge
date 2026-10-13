module;

#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

module forge.log.logger;

#include "details/logger_impl.hxx"

namespace forge {

logger::impl::impl(std::string name, logger parent)
    : _name{std::move(name)}, _parent{std::move(parent)} {}

std::vector<std::shared_ptr<sink>> logger::impl::targets() const {
   auto result = std::vector<std::shared_ptr<sink>>{};
   auto seen_loggers = std::unordered_set<const impl*>{};
   auto seen_sinks = std::unordered_set<sink*>{};

   // The caller holds log_mutex and retains the origin impl. Parent edges
   // cannot change during traversal, so every visited impl stays alive.
   for (auto current = this; current != nullptr && seen_loggers.insert(current).second;
        current = std::atomic_load_explicit(&current->_parent.my, std::memory_order_acquire).get()) {
      for (const auto& current_sink : current->_sinks) {
         if (current_sink && seen_sinks.insert(current_sink.get()).second) {
            result.push_back(current_sink);
         }
      }
   }
   return result;
}

} // namespace forge
