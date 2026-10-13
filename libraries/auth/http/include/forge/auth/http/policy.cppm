module;

#include <string>
#include <vector>

export module forge.auth.http.policy;

export import forge.auth.http.exceptions;
export import forge.auth.http.types;
export import forge.auth.session.session;
export import forge.net.http.cookie;

export namespace forge::auth::http {

[[nodiscard]] origin_policy make_origin_policy(std::vector<std::string> allowed_origins);
[[nodiscard]] session_evidence extract_session_evidence(const browser_request_evidence& request,
                                                        const cookie_policy& cookies);

[[nodiscard]] forge::auth::session::principal authorize(const session_evidence& evidence,
                                                        const forge::auth::session::session_record& session,
                                                        const forge::auth::credential::record& credential,
                                                        const authorization_options& options);

[[nodiscard]] forge::net::http::set_cookie
make_pre_session_cookie(const forge::crypto::core::secret_string& pre_session_token, const cookie_policy& policy);
[[nodiscard]] forge::net::http::set_cookie make_session_cookie(const forge::crypto::core::secret_string& session_token,
                                                               const cookie_policy& policy);
[[nodiscard]] forge::net::http::set_cookie make_csrf_cookie(const forge::crypto::core::secret_string& csrf_secret,
                                                            const cookie_policy& policy);
[[nodiscard]] forge::net::http::set_cookie make_clear_pre_session_cookie(const cookie_policy& policy);
[[nodiscard]] forge::net::http::set_cookie make_clear_session_cookie(const cookie_policy& policy);
[[nodiscard]] forge::net::http::set_cookie make_clear_csrf_cookie(const cookie_policy& policy);

void append_pre_session_cookie(forge::net::http::response& response,
                               const forge::crypto::core::secret_string& pre_session_token,
                               const cookie_policy& policy);
void append_session_cookies(forge::net::http::response& response,
                            const forge::auth::session::session_issuance& issuance, const cookie_policy& policy);
void append_approved_session_cookies(forge::net::http::response& response,
                                     const forge::crypto::core::secret_string& pre_session_token,
                                     const forge::auth::session::session_issuance& issuance,
                                     const cookie_policy& policy);
void append_rotated_session_cookies(forge::net::http::response& response,
                                    const forge::auth::session::session_issuance& issuance,
                                    const cookie_policy& policy);
void append_logout_cookies(forge::net::http::response& response, const cookie_policy& policy);

void apply_security_headers(forge::net::http::response& response, security_header_options options = {});

} // namespace forge::auth::http
