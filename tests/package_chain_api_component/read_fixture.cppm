module;

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

export module package.chain_api_component.read_fixture;

export import package.chain_api_component.test_support;

import forge.chain.api.block;
import forge.chain.api.info;
import forge.chain.api.state;

export namespace package_chain_api_component {

namespace chain_api = forge::chain::api;
namespace protocol = forge::chain::protocol;

[[nodiscard]] protocol::info_response make_info_response();
[[nodiscard]] protocol::block_state_response make_block_state_response(const protocol::info_response& source);
[[nodiscard]] protocol::table_changes_response make_table_changes_response(const protocol::info_response& source);

class info_implementation final : public chain_api::info {
 public:
   explicit info_implementation(protocol::info_response response);

   boost::asio::awaitable<protocol::info_response> get(protocol::anchored_request request) override;

   std::atomic<std::uint32_t> calls{0};
   std::atomic<protocol::audit_mode> last_audit{protocol::audit_mode::none};

 private:
   protocol::info_response response_;
};

class block_implementation : public chain_api::block {
 public:
   explicit block_implementation(protocol::block_state_response response);

   boost::asio::awaitable<protocol::block_response> get_block(protocol::block_request request) override;
   boost::asio::awaitable<protocol::block_header_response> get_header(protocol::block_request request) override;
   boost::asio::awaitable<protocol::block_state_response> get_block_state(protocol::block_request request) override;
   boost::asio::awaitable<protocol::block_range_response>
   get_canonical_range(protocol::block_range_request request) override;
   boost::asio::awaitable<protocol::protocol_features_response>
   get_activated_protocol_features(protocol::protocol_features_request request) override;
   boost::asio::awaitable<protocol::consensus_parameters_response>
   get_consensus_parameters(protocol::anchored_request request) override;
   boost::asio::awaitable<protocol::producers_response> get_producers(protocol::producers_request request) override;
   boost::asio::awaitable<protocol::producer_schedule_response>
   get_producer_schedule(protocol::anchored_request request) override;
   boost::asio::awaitable<protocol::finalizer_info_response> get_finalizer_info(protocol::anchored_request request) override;

   std::atomic<std::uint32_t> calls{0};
   std::atomic<protocol::audit_mode> last_audit{protocol::audit_mode::none};

 private:
   protocol::block_state_response response_;
};

class state_implementation : public chain_api::state {
 public:
   explicit state_implementation(protocol::table_changes_response response);

   boost::asio::awaitable<protocol::account_response> get_account(protocol::account_request request) override;
   boost::asio::awaitable<protocol::account_changes_response>
   get_account_changes(protocol::account_changes_request request) override;
   boost::asio::awaitable<protocol::code_response> get_code(protocol::code_request request) override;
   boost::asio::awaitable<protocol::permission_links_response>
   get_permission_links(protocol::permission_links_request request) override;
   boost::asio::awaitable<protocol::table_rows_response> get_table_rows(protocol::table_rows_request request) override;
   boost::asio::awaitable<protocol::table_changes_response>
   get_table_changes(protocol::table_changes_request request) override;
   boost::asio::awaitable<protocol::table_scope_response> get_table_scope(protocol::table_scope_request request) override;
   boost::asio::awaitable<protocol::currency_balance_response>
   get_currency_balance(protocol::currency_balance_request request) override;
   boost::asio::awaitable<protocol::currency_stats_response>
   get_currency_stats(protocol::currency_stats_request request) override;
   boost::asio::awaitable<protocol::scheduled_response>
   get_scheduled_transactions(protocol::scheduled_request request) override;
   boost::asio::awaitable<protocol::authorizers_response>
   get_accounts_by_authorizers(protocol::authorizers_request request) override;

   std::atomic<std::uint32_t> calls{0};
   std::atomic<protocol::audit_mode> last_audit{protocol::audit_mode::none};

 private:
   protocol::table_changes_response response_;
};

class read_services {
 public:
   read_services(std::shared_ptr<info_implementation> information, std::shared_ptr<block_implementation> blocks,
                 std::shared_ptr<state_implementation> state);

   [[nodiscard]] std::shared_ptr<chain_api::info> information() const;
   [[nodiscard]] std::shared_ptr<chain_api::block> blocks() const;
   [[nodiscard]] std::shared_ptr<chain_api::state> state() const;
   [[nodiscard]] std::uint32_t information_calls() const;
   [[nodiscard]] std::uint32_t block_calls() const;
   [[nodiscard]] std::uint32_t state_calls() const;
   [[nodiscard]] bool information_audit_required() const;
   [[nodiscard]] bool block_audit_required() const;
   [[nodiscard]] bool state_audit_required() const;

 private:
   std::shared_ptr<info_implementation> information_;
   std::shared_ptr<block_implementation> blocks_;
   std::shared_ptr<state_implementation> state_;
};

struct read_expectations {
   protocol::info_response information;
   protocol::block_state_response block;
   protocol::table_changes_response state;
};

[[nodiscard]] read_expectations make_read_expectations();
[[nodiscard]] read_services make_read_services(const read_expectations& expectations);

} // namespace package_chain_api_component
