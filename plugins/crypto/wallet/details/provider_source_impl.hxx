#pragma once

namespace forge::plugins::crypto::wallet {

class plugin::provider_source_impl final : public provider_source {
 public:
   explicit provider_source_impl(std::shared_ptr<impl> state);
   std::shared_ptr<forge::crypto::signer::provider> get(std::string name) override;

 private:
   std::shared_ptr<impl> state_;
};

} // namespace forge::plugins::crypto::wallet
