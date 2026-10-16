module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

export module forge.db.object.hooks;

import forge.db.ids.object_id;

export namespace forge::db::object {

enum class mutation_kind : std::uint8_t {
   insert = 1,
   replace = 2,
   modify = 3,
   erase = 4,
};

struct object_mutation {
   mutation_kind kind = mutation_kind::insert;
   forge::db::ids::object_id id;
   std::optional<std::vector<std::byte>> before;
   std::optional<std::vector<std::byte>> after;
};

struct allocation_change {
   forge::db::ids::object_id type;
   std::uint64_t before = 0;
   std::uint64_t after = 0;

   bool operator==(const allocation_change&) const = default;
};

struct change_set {
   std::vector<object_mutation> mutations;
   std::vector<allocation_change> allocations;

   [[nodiscard]] bool empty() const noexcept {
      return mutations.empty() && allocations.empty();
   }
};

class interceptor {
 public:
   virtual ~interceptor() = default;
   virtual boost::asio::awaitable<void> before_mutation(const object_mutation& mutation) = 0;
};

class observer {
 public:
   virtual ~observer() = default;
   virtual boost::asio::awaitable<void> after_commit(const change_set& changes) = 0;
};

class precommit_observer {
 public:
   virtual ~precommit_observer() = default;
   virtual boost::asio::awaitable<void> before_commit(const change_set& changes) = 0;
};

} // namespace forge::db::object
