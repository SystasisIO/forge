#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <variant>

import forge.plugins.swarm.node.api;
import forge.plugins.swarm.node.config;
import forge.plugins.swarm.node.exceptions;
import forge.plugins.swarm.node.plugin;
import forge.config.core.decode;
import forge.config.core.component;
import forge.config.core.document;
import forge.api.core.descriptor;

namespace sn = forge::plugins::swarm::node;

BOOST_AUTO_TEST_CASE(swarm_plugin_contract_is_local_and_has_only_native_dependencies) {
   BOOST_CHECK(sn::api::describe().supported_surfaces == forge::api::core::surface::local);
   BOOST_CHECK(sn::api::describe().methods.empty());
   const auto descriptor = sn::plugin::descriptor();
   BOOST_CHECK_EQUAL(descriptor.id.value, "forge.plugins.swarm.node");
   BOOST_REQUIRE_EQUAL(descriptor.dependencies.size(), 2U);
   BOOST_CHECK_EQUAL(descriptor.dependencies[0].value, "forge.plugins.net.p2p.node");
   BOOST_CHECK_EQUAL(descriptor.dependencies[1].value, "forge.plugins.db.store");
}

BOOST_AUTO_TEST_CASE(swarm_plugin_configuration_is_explicit_and_bounded) {
   auto settings = sn::config{};
   BOOST_CHECK(!settings.free_admission);
   BOOST_CHECK_NO_THROW(sn::validate_config(settings));
   settings.network.application_protocol = "/different/1";
   BOOST_CHECK_THROW(sn::validate_config(settings), sn::exceptions::invalid_config);
   settings = {};
   settings.network.profile.assign(129U, '/');
   BOOST_CHECK_THROW(sn::validate_config(settings), sn::exceptions::invalid_config);
   settings = {};
   settings.metadata.mode = "object";
   settings.metadata.object_name.clear();
   BOOST_CHECK_THROW(sn::validate_config(settings), sn::exceptions::invalid_config);
   settings = {};
   settings.metadata.max_inflight = 0;
   BOOST_CHECK_THROW(sn::validate_config(settings), sn::exceptions::invalid_config);
   settings.metadata.max_inflight = 33;
   BOOST_CHECK_THROW(sn::validate_config(settings), sn::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(swarm_plugin_programmatic_defaults_survive_app_default_materialization) {
   auto options = sn::plugin_options{};
   options.initial_config.free_admission = true;
   options.initial_config.network.profile = "/test/swarm/kad/1";
   auto plugin = sn::plugin{options};
   auto registry = forge::config::core::component_registry{};
   registry.add(*plugin.describe_config());
   const auto defaults = forge::config::core::defaults_for(registry);
   const auto decoded = forge::config::core::decode<sn::config>(defaults, "plugins.swarm.node");
   BOOST_REQUIRE(decoded.ok());
   BOOST_CHECK(decoded.value.free_admission);
   BOOST_CHECK_EQUAL(decoded.value.network.profile, options.initial_config.network.profile);
}
