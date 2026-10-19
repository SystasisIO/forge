module;

#include <boost/describe.hpp>
#include <forge/db/object/macros.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

export module forge.swarm.db.content;

export import forge.db.object.object;
export import forge.db.object.index;
export import forge.swarm.core.checkpoint;

export namespace forge::swarm::db {

enum class job_state : std::uint8_t {
   paused = 0,
   downloading = 1,
   seeding = 2,
   failed = 3,
};

struct content : forge::db::object::object<content, 81, 2> {
   core::swarm_id swarm;
   core::descriptor manifest;
   std::string locator;
   job_state state = job_state::paused;
   std::int32_t priority = 0;
   std::int64_t expires_at = std::numeric_limits<std::int64_t>::max();
   std::optional<core::resume_state> checkpoint;

   bool operator==(const content&) const = default;
};

struct by_id;
struct by_swarm;
struct by_locator;
struct by_state_priority;
struct by_expiry;

using content_index = forge::db::object::object_index<
   content,
   forge::db::object::indexed_by<
      forge::db::object::primary_unique<by_id>,
      forge::db::object::ordered_unique<
         by_swarm, forge::db::object::member<&content::swarm, &core::swarm_id::value>>,
      forge::db::object::ordered_unique<by_locator, forge::db::object::member<&content::locator>>,
      forge::db::object::ordered_non_unique<
         by_state_priority,
         forge::db::object::composite_key<
            forge::db::object::member<&content::state>,
            forge::db::object::descending<forge::db::object::member<&content::priority>>,
            forge::db::object::member<&content::swarm, &core::swarm_id::value>>>,
      forge::db::object::ordered_non_unique<
         by_expiry,
         forge::db::object::composite_key<
            forge::db::object::member<&content::expires_at>,
            forge::db::object::member<&content::swarm, &core::swarm_id::value>>>>>;

BOOST_DESCRIBE_ENUM(job_state, paused, downloading, seeding, failed)
BOOST_DESCRIBE_STRUCT(content, (forge::db::object::object<content, 81, 2>),
                      (swarm, manifest, locator, state, priority, expires_at, checkpoint))

} // namespace forge::swarm::db

export FORGE_DB_OBJECT(forge::swarm::db::content_index)
