module forge.plugins.chain.outbox.types;

forge::schema::object_schema<forge::plugins::chain::outbox::http_profile>
forge::schema::rules<forge::plugins::chain::outbox::http_profile>::define() {
   auto schema = forge::schema::object_schema<forge::plugins::chain::outbox::http_profile>{};
   schema.field<&forge::plugins::chain::outbox::http_profile::name>("name").required().non_empty();
   schema.field<&forge::plugins::chain::outbox::http_profile::url>("url").required().non_empty();
   return schema;
}

forge::schema::object_schema<forge::plugins::chain::outbox::config>
forge::schema::rules<forge::plugins::chain::outbox::config>::define() {
   auto schema = forge::schema::object_schema<forge::plugins::chain::outbox::config>{};
   schema.field<&forge::plugins::chain::outbox::config::store>("store").required().non_empty();
   schema.field<&forge::plugins::chain::outbox::config::http>("http")
       .items<forge::plugins::chain::outbox::http_profile>()
       .unique_by<&forge::plugins::chain::outbox::http_profile::name>();
   return schema;
}
