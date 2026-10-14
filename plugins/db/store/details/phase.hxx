#pragma once

namespace forge::plugins::db::store {

enum class phase : std::uint8_t {
   collecting,
   opening,
   ready,
   closed,
};

} // namespace forge::plugins::db::store
