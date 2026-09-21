module;

export module forge.auth.session.session;

export import forge.auth.session.exceptions;
export import forge.auth.session.types;

export namespace forge::auth::session {

[[nodiscard]] session_issuance issue_session(const forge::auth::pairing::credential& credential,
                                             session_options options);
void validate_issuance(const session_issuance& issuance);

[[nodiscard]] forge::crypto::digest::sha256
identify_session_token(const forge::crypto::core::secret_string& session_token);

[[nodiscard]] principal validate_session(const session_record& record,
                                         const forge::crypto::core::secret_string& session_token,
                                         const forge::auth::pairing::credential& credential, time_point now);

void verify_csrf_secret(const session_record& record, const forge::crypto::core::secret_string& csrf_secret,
                        time_point now);
void renew_idle(session_record& record, time_point now);

[[nodiscard]] session_issuance
rotate_session(session_record& record, const forge::auth::pairing::credential& credential, session_options options);
void logout_session(session_record& record, time_point now);
void revoke_session(session_record& record, time_point now);

[[nodiscard]] device_grant_issuance issue_device_grant(const forge::auth::pairing::credential& credential,
                                                       device_grant_options options);
void validate_device_grant_issuance(const device_grant_issuance& issuance);

[[nodiscard]] forge::crypto::digest::sha256
identify_device_grant_token(const forge::crypto::core::secret_string& token);

[[nodiscard]] forge::auth::pairing::credential_binding
validate_device_grant(const device_grant_record& record, const forge::crypto::core::secret_string& token,
                      const forge::auth::pairing::credential& credential, time_point now);

[[nodiscard]] device_grant_issuance rotate_device_grant(device_grant_record& record,
                                                        const forge::crypto::core::secret_string& token,
                                                        const forge::auth::pairing::credential& credential,
                                                        time_point now);
[[nodiscard]] device_grant_refresh refresh_device_grant(device_grant_record& record,
                                                        const forge::crypto::core::secret_string& token,
                                                        const forge::auth::pairing::credential& credential,
                                                        session_options options);
void revoke_device_grant(device_grant_record& record, time_point now);

} // namespace forge::auth::session
