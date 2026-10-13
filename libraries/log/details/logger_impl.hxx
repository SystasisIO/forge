#pragma once

namespace forge {

class logger::impl {
 public:
   impl(std::string name = {}, logger parent = nullptr);

   // All route state, including parent links, is protected by log_mutex.
   [[nodiscard]] std::vector<std::shared_ptr<sink>> targets() const;

   std::string _name;
   logger _parent;
   bool _enabled = true;
   log_level _level = log_level::warn;
   std::vector<std::shared_ptr<sink>> _sinks;
};

} // namespace forge
