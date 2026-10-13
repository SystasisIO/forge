module;

#include <utility>

export module forge.auth.pairing.pairing;

export import forge.auth.pairing.exceptions;
export import forge.auth.pairing.types;

export namespace forge::auth::pairing {

[[nodiscard]] bootstrap_issuance begin_bootstrap(bootstrap_options options);

[[nodiscard]] token_digest
identify_bootstrap_token(const forge::crypto::core::secret_string& bootstrap_token);

[[nodiscard]] pending_issuance consume_bootstrap(bootstrap_record& bootstrap,
                                                 const forge::crypto::core::secret_string& token,
                                                 pairing_request request, consume_options options);

[[nodiscard]] pending_issuance supersede_pending(pending_request& pending, pairing_request replacement, time_point now);

[[nodiscard]] token_digest identify_pre_session(const forge::crypto::core::secret_string& pre_session_token);
void validate_pre_session(const pending_request& pending, const forge::crypto::core::secret_string& pre_session_token,
                          time_point now);
[[nodiscard]] forge::auth::credential::binding
consume_approved_pre_session(pending_request& pending, const forge::crypto::core::secret_string& pre_session_token,
                             time_point now);

[[nodiscard]] forge::auth::credential::record approve_pending(pending_request& pending, approval_options options);
void reject_pending(pending_request& pending, time_point now);

} // namespace forge::auth::pairing
