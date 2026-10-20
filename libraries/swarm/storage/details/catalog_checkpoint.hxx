#pragma once

namespace forge::swarm::storage {

class catalog::checkpoint final : public core::checkpoint_store {
 public:
   struct binding {
      db::content::id_t row;
      core::swarm_id swarm;
      std::string locator;
      core::descriptor manifest;
   };

   checkpoint(std::shared_ptr<impl>, db::content);
   ~checkpoint() override;
   boost::asio::awaitable<std::optional<core::resume_state>> async_load(core::swarm_id, std::stop_token) override;
   boost::asio::awaitable<void> async_save(core::resume_state, std::stop_token) override;

 private:
   static void validate_binding(const db::content&, const binding&);
   static boost::asio::awaitable<std::optional<core::resume_state>> load(
      std::shared_ptr<impl>, std::shared_ptr<const binding>, core::swarm_id, std::stop_token);
   static boost::asio::awaitable<void> save(
      std::shared_ptr<impl>, std::shared_ptr<const binding>, core::resume_state, std::stop_token);

   const std::shared_ptr<impl> _owner;
   const std::shared_ptr<const binding> _binding;
};

} // namespace forge::swarm::storage
