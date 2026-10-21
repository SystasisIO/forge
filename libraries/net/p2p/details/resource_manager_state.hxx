#pragma once

namespace forge::net::p2p {

struct resource_manager::ledger {
   enum class kind { lifecycle, connection, stream };

   std::uint64_t id = 0;
   kind value_kind = kind::lifecycle;
   session_direction direction = session_direction::outbound;
   scope_totals usage;
   std::optional<peer_id> peer;
   std::optional<protocol_id> protocol;
   std::optional<std::string> service;
   bool parent_active = true;
   bool transient = false;
};

struct resource_manager::dial_ledger {
   std::optional<peer_id> peer;
   bool active = true;
};

struct resource_manager::state {
   struct ledger_attempt {
      std::shared_ptr<ledger> reservation;
      transition_result outcome = transition_result::runtime_failure;
   };

   struct dial_attempt {
      std::shared_ptr<dial_ledger> reservation;
      transition_result outcome = transition_result::runtime_failure;
   };

   explicit state(limits value) noexcept;

   [[nodiscard]] const limits& configured_limits() const noexcept;
   [[nodiscard]] snapshot current() const noexcept;
   [[nodiscard]] ledger_attempt reserve_lifecycle() noexcept;
   [[nodiscard]] ledger_attempt reserve_session(session_direction direction) noexcept;
   [[nodiscard]] ledger_attempt reserve_stream(peer_id peer, session_direction direction) noexcept;
   [[nodiscard]] dial_attempt reserve_dial() noexcept;
   [[nodiscard]] dial_attempt reserve_dial(peer_id peer) noexcept;
   [[nodiscard]] bool dial_active(const std::shared_ptr<dial_ledger>& value) const noexcept;
   [[nodiscard]] bool dial_bound(const std::shared_ptr<dial_ledger>& value) const noexcept;
   [[nodiscard]] transition_result bind_dial(const std::shared_ptr<dial_ledger>& value, peer_id peer) noexcept;
   void release_dial(const std::shared_ptr<dial_ledger>& value) noexcept;
   [[nodiscard]] transition_result reserve_relay(const peer_id& peer) noexcept;
   void release_relay(const peer_id& peer) noexcept;
   [[nodiscard]] transition_result record_malformed(const peer_id& peer) noexcept;
   [[nodiscard]] bool session_established(const std::shared_ptr<ledger>& value) const noexcept;
   [[nodiscard]] bool stream_bound(const std::shared_ptr<ledger>& value) const noexcept;
   [[nodiscard]] bool stream_service_bound(const std::shared_ptr<ledger>& value) const noexcept;
   [[nodiscard]] transition_result establish_session(const std::shared_ptr<ledger>& value, session_scope scope) noexcept;
   [[nodiscard]] transition_result bind_protocol(const std::shared_ptr<ledger>& value,
                                                  const protocol_id& protocol) noexcept;
   [[nodiscard]] transition_result bind_service(const std::shared_ptr<ledger>& value,
                                                 std::string_view service) noexcept;
   [[nodiscard]] bool reserve_memory(const std::shared_ptr<ledger>& value, std::uint64_t bytes,
                                     memory_priority priority) noexcept;
   [[nodiscard]] bool reserve_file_descriptors(const std::shared_ptr<ledger>& value, std::size_t count) noexcept;
   void release_memory(const std::shared_ptr<ledger>& value, std::uint64_t bytes) noexcept;
   void release_file_descriptors(const std::shared_ptr<ledger>& value, std::size_t count) noexcept;
   void release_parent(const std::shared_ptr<ledger>& value) noexcept;

 private:
   struct scope_account {
      scope_totals usage;
   };

   struct memory_rejection {
      const char* reason = "missing_scope";
      const char* scope = "unknown";
      const char* dimension = "none";
      std::uint64_t usage = 0;
      std::uint64_t limit = 0;
      std::uint64_t threshold = 0;
   };

   [[nodiscard]] ledger_attempt make_ledger_locked() noexcept;
   [[nodiscard]] dial_attempt reserve_dial_locked(std::optional<peer_id> peer) noexcept;
   [[nodiscard]] bool reject_limit_locked(std::uint64_t& reason) noexcept;
   [[nodiscard]] bool reject_invalid_transition_locked() noexcept;
   void record_runtime_failure_locked() noexcept;
   [[nodiscard]] bool can_add_locked(const scope_account& account, const scope_limits& limits,
                                     const scope_totals& delta, memory_priority priority) const noexcept;
   [[nodiscard]] bool can_remove_locked(const scope_account& account, const scope_totals& delta) const noexcept;
   [[nodiscard]] bool can_add_to_current_scopes_locked(const ledger& value, const scope_totals& delta,
                                                       memory_priority priority,
                                                       memory_rejection* rejected = nullptr) const noexcept;
   [[nodiscard]] bool can_remove_from_current_scopes_locked(const ledger& value,
                                                            const scope_totals& delta) const noexcept;
   [[nodiscard]] transition_result bind_service_locked(const std::shared_ptr<ledger>& value,
                                                       std::string_view service) noexcept;
   void add_locked(scope_account& account, const scope_totals& delta) noexcept;
   void remove_locked(scope_account& account, const scope_totals& delta) noexcept;
   void add_to_current_scopes_locked(const ledger& value, const scope_totals& delta) noexcept;
   void remove_from_current_scopes_locked(const ledger& value, const scope_totals& delta) noexcept;
   void cleanup_scopes_locked(const ledger& value) noexcept;

   mutable std::mutex mutex_;
   limits limits_;
   snapshot snapshot_;
   scope_account system_;
   scope_account transient_;
   scope_account connections_;
   scope_account streams_;
   std::map<peer_id, scope_account> peers_;
   std::map<std::string, scope_account> protocols_;
   std::map<std::string, scope_account> services_;
   std::map<peer_id, std::map<std::string, scope_account>> protocol_peers_;
   std::map<peer_id, std::map<std::string, scope_account>> service_peers_;
   std::map<peer_id, std::size_t> dial_attempts_by_peer_;
   std::map<peer_id, std::size_t> relay_reservations_by_peer_;
   std::map<peer_id, std::size_t> malformed_by_peer_;
   std::uint64_t next_ledger_id_ = 1;
};

} // namespace forge::net::p2p

namespace forge::net::p2p::detail {

void fail_next_service_bind_prepare_for_test() noexcept;
void fail_next_session_establish_prepare_for_test() noexcept;
void fail_next_dial_bind_prepare_for_test() noexcept;
void fail_next_malformed_record_prepare_for_test() noexcept;
void fail_next_lifecycle_reserve_prepare_for_test() noexcept;
void fail_next_session_reserve_prepare_for_test() noexcept;
void fail_next_dial_reserve_prepare_for_test() noexcept;
void fail_next_stream_reserve_prepare_for_test() noexcept;
void fail_next_relay_reserve_prepare_for_test() noexcept;

} // namespace forge::net::p2p::detail
