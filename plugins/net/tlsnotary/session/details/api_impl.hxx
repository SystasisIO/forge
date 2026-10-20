#pragma once

namespace forge::plugins::net::tlsnotary::session {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<plugin::impl> impl);
   boost::asio::awaitable<forge::net::tlsnotary::result> prove(forge::net::tlsnotary::prover_request request) override;
   boost::asio::awaitable<forge::net::tlsnotary::result>
   verify(forge::net::tlsnotary::verifier_request request) override;
   bool cancel(std::uint64_t id) noexcept override;
   forge::net::tlsnotary::metrics snapshot() const override;

 private:
   std::shared_ptr<plugin::impl> _impl;
};
} // namespace forge::plugins::net::tlsnotary::session
