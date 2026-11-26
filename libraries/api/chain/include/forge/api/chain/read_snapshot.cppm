module;

#include <boost/asio/awaitable.hpp>
#include <functional>
#include <memory>
#include <optional>

export module forge.api.chain.read_snapshot;

export import forge.chain.protocol.state_query;

export namespace forge::api::chain {

// A typed read borrows this verified view for the duration of its invocation.
class read_snapshot {
 public:
   // Keep the key private to the owner of one verification rule.
   class check {
    public:
      check();
      check(const check&) = delete;
      check& operator=(const check&) = delete;

    private:
      friend class read_snapshot;
      struct identity {};
      std::shared_ptr<const identity> _id;
   };
   using rows_reader = std::function<boost::asio::awaitable<::forge::chain::protocol::table_rows_response>(
       ::forge::chain::protocol::table_rows_request)>;
   using code_reader = std::function<boost::asio::awaitable<::forge::chain::protocol::code_response>(
       ::forge::chain::protocol::code_request)>;

   using contract_caller = std::function<boost::asio::awaitable<::forge::chain::protocol::contract_call_response>(
       ::forge::chain::protocol::contract_call_request)>;

   read_snapshot(::forge::chain::protocol::state_anchor anchor, ::forge::chain::protocol::time_point_sec time,
                 rows_reader rows, code_reader code, contract_caller trusted = {});

   [[nodiscard]] const ::forge::chain::protocol::state_anchor& anchor() const noexcept {
      return _anchor;
   }
   [[nodiscard]] ::forge::chain::protocol::time_point_sec time() const noexcept {
      return _time;
   }
   [[nodiscard]] bool verified(const check& rule) const noexcept;
   void confirm(const check& rule) const noexcept;

   boost::asio::awaitable<::forge::chain::protocol::table_rows_response>
   get_table_rows(::forge::chain::protocol::table_rows_request request) const;
   boost::asio::awaitable<::forge::chain::protocol::code_response>
   get_code(::forge::chain::protocol::code_request request) const;

   // Execute against this snapshot at an explicitly trusted destination.
   boost::asio::awaitable<std::optional<::forge::chain::protocol::bytes>>
   call(::forge::chain::protocol::account_name contract, ::forge::chain::protocol::bytes data) const;

   read_snapshot(const read_snapshot&) = delete;
   read_snapshot& operator=(const read_snapshot&) = delete;
   read_snapshot(read_snapshot&&) = default;

 private:
   ::forge::chain::protocol::state_anchor _anchor;
   ::forge::chain::protocol::time_point_sec _time;
   rows_reader _rows;
   code_reader _code;
   contract_caller _trusted;
   mutable std::shared_ptr<const check::identity> _verified;
};

} // namespace forge::api::chain
