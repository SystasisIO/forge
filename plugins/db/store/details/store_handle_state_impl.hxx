#pragma once

namespace forge::plugins::db::store {

class plugin::store_handle_state_impl final : public store_handle_state {
 public:
   store_handle_state_impl(std::weak_ptr<impl> owner, std::string name);

   [[nodiscard]] std::string name() const override;
   [[nodiscard]] std::shared_ptr<forge::db::object::store> require_objects() const override;
   [[nodiscard]] std::shared_ptr<forge::db::blob::store> require_blobs() const override;
   [[nodiscard]] std::shared_ptr<forge::db::revision::store> require_revisions() const override;
   boost::asio::awaitable<transaction> begin_transaction() const override;

 private:
   boost::asio::awaitable<void> close_for_activation() const override;
   void claim_activation() const override;
   boost::asio::awaitable<void> activate_closed(std::filesystem::path destination) const override;
   boost::asio::awaitable<void> confirm_activation() const override;
   [[nodiscard]] activation_status closed_state() const override;
   [[nodiscard]] bool activation_inflight() const noexcept override;
   [[nodiscard]] std::shared_ptr<impl> activation_owner() const;
   [[nodiscard]] std::shared_ptr<forge::db::core::driver> require_driver() const override;

   std::weak_ptr<impl> owner_;
   std::string name_;
   mutable std::mutex activation_mutex_;
   mutable std::shared_ptr<impl> activation_owner_;
};

} // namespace forge::plugins::db::store
