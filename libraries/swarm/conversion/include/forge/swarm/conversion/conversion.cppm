module;

#include <optional>

export module forge.swarm.conversion.conversion;

export import forge.core.conversion;
export import forge.swarm.db.content;
export import forge.swarm.protocol.metadata;

export namespace forge::core {

template <> class conversion<forge::swarm::protocol::metadata> {
 public:
   explicit conversion(const forge::swarm::db::content&);
   [[nodiscard]] forge::swarm::protocol::metadata finish() &&;

 private:
   std::optional<forge::swarm::protocol::metadata> _value;
};

template <> class conversion<forge::swarm::core::descriptor> {
 public:
   explicit conversion(forge::swarm::protocol::metadata);
   [[nodiscard]] forge::swarm::core::descriptor finish() &&;

 private:
   std::optional<forge::swarm::core::descriptor> _value;
};

} // namespace forge::core
