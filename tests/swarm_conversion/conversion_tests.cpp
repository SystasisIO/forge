#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

import forge.swarm.conversion.conversion;
import forge.raw.raw;

namespace conversion_test {
namespace core = forge::swarm::core;
namespace protocol = forge::swarm::protocol;
namespace db = forge::swarm::db;

template <typename T>
concept complete_conversion = requires { sizeof(forge::core::conversion<T>); };
template <typename T>
concept has_local_fields = requires(T value) { value.id; } || requires(T value) { value.locator; } ||
                           requires(T value) { value.state; } || requires(T value) { value.checkpoint; } ||
                           requires(T value) { value.priority; } || requires(T value) { value.expires_at; };

static_assert(!complete_conversion<db::content>);
static_assert(!has_local_fields<protocol::metadata>);

db::content row() {
   const auto bytes = std::array<std::uint8_t, 1>{'a'};
   auto value = db::content{};
   value.id = db::content::id_t{7U};
   value.manifest = {core::descriptor_version::v1,
                     core::block_size,
                     {{{"a"}, 1U, core::hash_file(bytes, core::block_size).root, {}}}};
   value.swarm = core::calculate_swarm_id(value.manifest);
   value.locator = "private/local/location";
   value.state = db::job_state::downloading;
   value.priority = 9;
   value.expires_at = 12345;
   value.checkpoint = core::resume_state{1U, value.swarm, 4U, false, {0U}, {{{0U, 0U}, {0U}}}};
   return value;
}

protocol::metadata export_metadata(const db::content& value) {
   return forge::core::conversion<protocol::metadata>{value}.finish();
}

} // namespace conversion_test

BOOST_AUTO_TEST_SUITE(swarm_conversion)

BOOST_AUTO_TEST_CASE(partial_metadata_exports_and_imports_only_the_validated_descriptor) {
   using namespace conversion_test;
   const auto local = row();
   BOOST_CHECK(!core::bitmap_test(local.checkpoint->verified, 0U));
   const auto remote = export_metadata(local);
   BOOST_CHECK(remote.swarm == local.swarm);
   BOOST_CHECK(remote.manifest == local.manifest);
   const auto imported = forge::core::conversion<core::descriptor>{remote}.finish();
   BOOST_CHECK(imported == local.manifest);
   const auto public_bytes = forge::raw::pack(remote);
   auto expected = forge::raw::pack(local.swarm);
   const auto descriptor_bytes = core::encode_descriptor(local.manifest);
   expected.insert(expected.end(), descriptor_bytes.begin(), descriptor_bytes.end());
   BOOST_CHECK(public_bytes == expected);
   BOOST_TEST(public_bytes.size() < forge::raw::pack(local).size());
}

BOOST_AUTO_TEST_CASE(all_valid_job_states_and_private_metadata_variations_have_identical_public_bytes) {
   using namespace conversion_test;
   auto local = row();
   const auto expected = forge::raw::pack(export_metadata(local));
   for (const auto state :
        {db::job_state::paused, db::job_state::downloading, db::job_state::seeding, db::job_state::failed}) {
      local.state = state;
      local.id = db::content::id_t{std::numeric_limits<std::uint64_t>::max()};
      local.locator = "another/private/location";
      local.priority = std::numeric_limits<std::int32_t>::min();
      local.expires_at = std::numeric_limits<std::int64_t>::max();
      local.checkpoint->generation += 1U;
      BOOST_CHECK(forge::raw::pack(export_metadata(local)) == expected);
   }
   local.checkpoint.reset();
   BOOST_CHECK(forge::raw::pack(export_metadata(local)) == expected);
}

BOOST_AUTO_TEST_CASE(invalid_local_rows_and_checkpoint_relationships_are_rejected_without_mutation) {
   using namespace conversion_test;
   for (auto mutation = 0U; mutation < 13U; ++mutation) {
      auto local = row();
      if (mutation == 0U)
         local.swarm = {};
      if (mutation == 1U)
         local.manifest.files[0].path = {".."};
      if (mutation == 2U)
         local.manifest.version = static_cast<core::descriptor_version>(9U);
      if (mutation == 3U)
         local.locator.clear();
      if (mutation == 4U)
         local.locator.resize(4097U, 'x');
      if (mutation == 5U)
         local.locator.push_back('\0');
      if (mutation == 6U)
         local.expires_at = -1;
      if (mutation == 7U)
         local.state = static_cast<db::job_state>(255U);
      if (mutation == 8U)
         local.checkpoint->id = {};
      if (mutation == 9U)
         local.checkpoint->verified = {2U};
      if (mutation == 10U)
         local.checkpoint->published = true;
      if (mutation == 11U)
         local.checkpoint->generation = std::numeric_limits<std::uint64_t>::max();
      if (mutation == 12U)
         local.checkpoint->unfinished[0].piece.index = 1U;
      const auto original = local;
      BOOST_CHECK_THROW((void)export_metadata(local), protocol::exceptions::invalid_metadata);
      BOOST_CHECK(local == original);
   }
}

BOOST_AUTO_TEST_CASE(remote_import_rejects_mismatched_identity_or_malformed_descriptor_and_has_no_db_mapping) {
   using namespace conversion_test;
   auto remote = export_metadata(row());
   remote.swarm = {};
   BOOST_CHECK_THROW((forge::core::conversion<core::descriptor>{remote}), protocol::exceptions::invalid_metadata);
   remote = export_metadata(row());
   remote.manifest.files[0].size = core::max_file_size + 1U;
   BOOST_CHECK_THROW((forge::core::conversion<core::descriptor>{remote}), protocol::exceptions::invalid_metadata);
   BOOST_CHECK(!complete_conversion<db::content>);
   BOOST_CHECK(!has_local_fields<protocol::metadata>);
}

BOOST_AUTO_TEST_CASE(conversion_finish_is_rvalue_only_and_one_shot) {
   using namespace conversion_test;
   auto outgoing = forge::core::conversion<protocol::metadata>{row()};
   const auto metadata = std::move(outgoing).finish();
   BOOST_CHECK_THROW((void)std::move(outgoing).finish(), protocol::exceptions::invalid_metadata);
   auto incoming = forge::core::conversion<core::descriptor>{metadata};
   BOOST_CHECK(std::move(incoming).finish() == metadata.manifest);
   BOOST_CHECK_THROW((void)std::move(incoming).finish(), protocol::exceptions::invalid_metadata);
}

BOOST_AUTO_TEST_SUITE_END()
