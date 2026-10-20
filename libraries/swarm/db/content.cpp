module;

#include <string>

module forge.swarm.db.content;

namespace forge::swarm::db {

core::sha256 content::locator_key() const { return core::sha256::hash(locator); }

} // namespace forge::swarm::db
