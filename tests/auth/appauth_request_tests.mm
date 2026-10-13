#include <boost/test/unit_test.hpp>

#include "../../libraries/auth/appauth/details/auth_state_binding.hxx"

#import <AppAuth.h>
#import <Foundation/Foundation.h>
#import "../../vendor/appauth/Sources/AppAuth/macOS/LoopbackHTTPServer/OIDLoopbackHTTPServer.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

@interface FORGEAppAuthTestUserAgent : NSObject <OIDExternalUserAgent>
@end

@implementation FORGEAppAuthTestUserAgent

- (BOOL)presentExternalUserAgentRequest:(id<OIDExternalUserAgentRequest>)request
                                session:(id<OIDExternalUserAgentSession>)session {
   return YES;
}

- (void)dismissExternalUserAgentAnimated:(BOOL)animated completion:(void (^)(void))completion {
   if (completion != nil) {
      completion();
   }
}

@end

@interface FORGEAppAuthTestHTTPDelegate : NSObject <NSURLSessionTaskDelegate>
@end

@implementation FORGEAppAuthTestHTTPDelegate

- (void)URLSession:(NSURLSession*)session
                         task:(NSURLSessionTask*)task
   willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                   newRequest:(NSURLRequest*)request
            completionHandler:(void (^)(NSURLRequest*))completion {
   // Observe the loopback 302 itself; never contact the configured success page.
   completion(nil);
}

@end

// Exercise the pinned SDK's request handler directly, without a browser or a token endpoint.
@interface OIDRedirectHTTPHandler (FORGEAppAuthRequestTests)
- (void)HTTPConnection:(HTTPConnection*)connection didReceiveRequest:(HTTPServerRequest*)request;
@end

namespace {

struct loopback_response {
   NSHTTPURLResponse* response = nil;
   NSData* body = nil;
   NSError* error = nil;
   bool completed = false;
   bool waiting = true;
};

[[nodiscard]] std::shared_ptr<loopback_response> receive_loopback_response(NSURL* url) {
   BOOST_REQUIRE([NSThread isMainThread]);
   auto result = std::make_shared<loopback_response>();
   auto configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
   configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
   configuration.URLCache = nil;
   configuration.connectionProxyDictionary = @{};
   configuration.timeoutIntervalForRequest = 5;
   configuration.timeoutIntervalForResource = 5;
   auto delegate = [[FORGEAppAuthTestHTTPDelegate alloc] init];
   auto session = [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:nil];
   auto task = [session dataTaskWithURL:url
                    completionHandler:^(NSData* body, NSURLResponse* response, NSError* error) {
                      // All result mutation and run-loop stopping happen on the main thread.
                      CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                        if (!result->waiting) {
                           return;
                        }
                        if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                           result->response = (NSHTTPURLResponse*)response;
                        }
                        result->body = body;
                        result->error = error;
                        result->completed = true;
                        CFRunLoopStop(CFRunLoopGetMain());
                      });
                      CFRunLoopWakeUp(CFRunLoopGetMain());
                    }];
   // Keep the mode runnable after the one-shot listener removes its socket sources.
   auto source_context = CFRunLoopSourceContext{};
   source_context.perform = [](void*) {};
   auto waiting_source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &source_context);
   CFRunLoopAddSource(CFRunLoopGetMain(), waiting_source, kCFRunLoopDefaultMode);
   [task resume];

   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while (!result->completed) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
         break;
      }
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>{remaining}.count(), false);
   }
   // A late canceled completion must not stop a subsequent test's run loop.
   result->waiting = false;
   [session invalidateAndCancel];
   CFRunLoopRemoveSource(CFRunLoopGetMain(), waiting_source, kCFRunLoopDefaultMode);
   CFRelease(waiting_source);
   return result;
}

[[nodiscard]] HTTPServerRequest* http_request(NSURL* url) {
   auto raw = CFHTTPMessageCreateRequest(kCFAllocatorDefault, CFSTR("GET"), (__bridge CFURLRef)url,
                                        kCFHTTPVersion1_1);
   auto request = [[HTTPServerRequest alloc] initWithRequest:raw connection:nil];
   CFRelease(raw);
   return request;
}

[[nodiscard]] std::pair<int, int> connect_to_loopback(NSURL* url) {
   const auto ipv4 = [url.host isEqualToString:@"127.0.0.1"];
   BOOST_REQUIRE(ipv4 || [url.host isEqualToString:@"::1"] || [url.host isEqualToString:@"[::1]"]);
   const auto socket = ::socket(ipv4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP);
   BOOST_REQUIRE(socket >= 0);
   auto result = 0;
   if (ipv4) {
      auto address = sockaddr_in{};
      address.sin_len = sizeof(address);
      address.sin_family = AF_INET;
      address.sin_port = htons(url.port.unsignedShortValue);
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      result = ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
   } else {
      auto address = sockaddr_in6{};
      address.sin6_len = sizeof(address);
      address.sin6_family = AF_INET6;
      address.sin6_port = htons(url.port.unsignedShortValue);
      address.sin6_addr = in6addr_loopback;
      result = ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
   }
   const auto error = errno;
   ::close(socket);
   return {result, error};
}

} // namespace

BOOST_AUTO_TEST_SUITE(appauth_request_tests)

BOOST_AUTO_TEST_CASE(native_code_flow_uses_state_nonce_and_pkce_s256) {
   @autoreleasepool {
      auto configuration = [[OIDServiceConfiguration alloc]
          initWithAuthorizationEndpoint:[NSURL URLWithString:@"https://issuer.example/authorize"]
                          tokenEndpoint:[NSURL URLWithString:@"https://issuer.example/token"]];
      auto request = [[OIDAuthorizationRequest alloc]
          initWithConfiguration:configuration
                       clientId:@"native-client"
                         scopes:@[ @"openid", @"profile" ]
                    redirectURL:[NSURL URLWithString:@"http://127.0.0.1:49152/callback"]
                   responseType:OIDResponseTypeCode
           additionalParameters:@{@"audience" : @"forge-api"}];

      BOOST_REQUIRE(request != nil);
      BOOST_CHECK(request.clientSecret == nil);
      BOOST_REQUIRE(request.state != nil);
      BOOST_REQUIRE(request.nonce != nil);
      BOOST_REQUIRE(request.codeVerifier != nil);
      BOOST_REQUIRE(request.codeChallenge != nil);
      BOOST_CHECK(request.state.length >= 32U);
      BOOST_CHECK(request.nonce.length >= 32U);
      BOOST_CHECK(request.codeVerifier.length >= 43U);
      BOOST_CHECK(request.codeVerifier.length <= 128U);
      BOOST_CHECK([request.codeChallengeMethod isEqualToString:OIDOAuthorizationRequestCodeChallengeMethodS256]);
      BOOST_CHECK([request.codeChallenge
          isEqualToString:[OIDAuthorizationRequest codeChallengeS256ForVerifier:request.codeVerifier]]);
      BOOST_CHECK([request.additionalParameters[@"audience"] isEqualToString:@"forge-api"]);
   }
}

BOOST_AUTO_TEST_CASE(loopback_response_and_authorization_completion_have_separate_lifetimes) {
   @autoreleasepool {
      BOOST_REQUIRE([NSThread isMainThread]);
      auto success_url = [NSURL URLWithString:@"https://success.example/complete"];
      auto handler = [[OIDRedirectHTTPHandler alloc] initWithSuccessURL:success_url];
      NSError* listener_error = nil;
      auto redirect = [handler startHTTPListener:&listener_error];
      BOOST_REQUIRE(redirect != nil);
      BOOST_REQUIRE(listener_error == nil);

      auto configuration = [[OIDServiceConfiguration alloc]
          initWithAuthorizationEndpoint:[NSURL URLWithString:@"https://issuer.example/authorize"]
                          tokenEndpoint:[NSURL URLWithString:@"https://issuer.example/token"]];
      auto request = [[OIDAuthorizationRequest alloc] initWithConfiguration:configuration
                                                                  clientId:@"native-client"
                                                                    scopes:@[ @"openid", @"profile" ]
                                                               redirectURL:redirect
                                                              responseType:OIDResponseTypeCode
                                                      additionalParameters:nil];
      __block auto callbacks = std::size_t{0};
      __block auto authorized = false;
      __block auto response_pending_at_completion = false;
      __block HTTPServerRequest* callback_request = nil;
      auto agent = [[FORGEAppAuthTestUserAgent alloc] init];
      // Forge waits for code exchange too; this lower-level callback proves no HTTP delivery barrier exists here.
      auto flow = [OIDAuthorizationService
          presentAuthorizationRequest:request
                    externalUserAgent:agent
                             callback:^(OIDAuthorizationResponse* response, NSError* error) {
                               ++callbacks;
                               authorized = response != nil && error == nil;
                               response_pending_at_completion = callback_request.response == nullptr;
                             }];
      handler.currentAuthorizationFlow = flow;

      auto unrelated = http_request([redirect URLByAppendingPathComponent:@"unrelated"]);
      [handler HTTPConnection:nil didReceiveRequest:unrelated];
      BOOST_REQUIRE(unrelated.response != nullptr);
      BOOST_CHECK_EQUAL(CFHTTPMessageGetResponseStatusCode(unrelated.response), 404);
      BOOST_CHECK_EQUAL(callbacks, 0U);
      BOOST_CHECK(handler.currentAuthorizationFlow == flow);

      auto callback_url = [NSURLComponents componentsWithURL:redirect resolvingAgainstBaseURL:YES];
      callback_url.queryItems = @[
         [NSURLQueryItem queryItemWithName:@"code" value:@"fixture-code"],
         [NSURLQueryItem queryItemWithName:@"state" value:request.state],
      ];
      callback_request = http_request(callback_url.URL);
      [handler HTTPConnection:nil didReceiveRequest:callback_request];

      BOOST_CHECK_EQUAL(callbacks, 1U);
      BOOST_CHECK(authorized);
      BOOST_CHECK(response_pending_at_completion);
      BOOST_CHECK(handler.currentAuthorizationFlow == nil);
      BOOST_REQUIRE(callback_request.response != nullptr);
      BOOST_CHECK_EQUAL(CFHTTPMessageGetResponseStatusCode(callback_request.response), 302);
      NSString* location = (__bridge_transfer NSString*)CFHTTPMessageCopyHeaderFieldValue(
          callback_request.response, CFSTR("Location"));
      BOOST_CHECK([location isEqualToString:success_url.absoluteString]);

      const auto [connected, error] = connect_to_loopback(redirect);
      BOOST_CHECK_EQUAL(connected, -1);
      BOOST_CHECK_EQUAL(error, ECONNREFUSED);

      [handler cancelHTTPListener];
      [flow cancel];
      BOOST_CHECK_EQUAL(callbacks, 1U);
   }
}

BOOST_AUTO_TEST_CASE(loopback_tcp_delivers_the_complete_first_success_redirect) {
   @autoreleasepool {
      BOOST_REQUIRE([NSThread isMainThread]);
      auto success_url = [NSURL URLWithString:@"https://success.example/complete"];
      auto handler = [[OIDRedirectHTTPHandler alloc] initWithSuccessURL:success_url];
      NSError* listener_error = nil;
      auto redirect = [handler startHTTPListener:&listener_error];
      BOOST_REQUIRE(redirect != nil);
      BOOST_REQUIRE(listener_error == nil);

      auto configuration = [[OIDServiceConfiguration alloc]
          initWithAuthorizationEndpoint:[NSURL URLWithString:@"https://issuer.example/authorize"]
                          tokenEndpoint:[NSURL URLWithString:@"https://issuer.example/token"]];
      auto request = [[OIDAuthorizationRequest alloc] initWithConfiguration:configuration
                                                                  clientId:@"native-client"
                                                                    scopes:@[ @"openid", @"profile" ]
                                                               redirectURL:redirect
                                                              responseType:OIDResponseTypeCode
                                                      additionalParameters:nil];
      __block auto callbacks = std::size_t{0};
      __block auto authorized = false;
      auto agent = [[FORGEAppAuthTestUserAgent alloc] init];
      auto flow = [OIDAuthorizationService
          presentAuthorizationRequest:request
                    externalUserAgent:agent
                             callback:^(OIDAuthorizationResponse* response, NSError* error) {
                               ++callbacks;
                               authorized = response != nil && error == nil;
                             }];
      handler.currentAuthorizationFlow = flow;

      auto callback_url = [NSURLComponents componentsWithURL:redirect resolvingAgainstBaseURL:YES];
      callback_url.queryItems = @[
         [NSURLQueryItem queryItemWithName:@"code" value:@"fixture-code"],
         [NSURLQueryItem queryItemWithName:@"state" value:request.state],
      ];
      // Unlike the direct-handler test, this traverses TCP accept, streams and the SDK's response writer.
      auto received = receive_loopback_response(callback_url.URL);
      BOOST_REQUIRE(received->completed);
      BOOST_REQUIRE(received->error == nil);
      BOOST_REQUIRE(received->response != nil);
      BOOST_CHECK_EQUAL(received->response.statusCode, 302);
      auto location = [received->response valueForHTTPHeaderField:@"Location"];
      auto content_length = [received->response valueForHTTPHeaderField:@"Content-Length"];
      BOOST_CHECK([location isEqualToString:success_url.absoluteString]);
      BOOST_REQUIRE(content_length != nil);
      BOOST_REQUIRE(received->body != nil);
      const auto declared_length = content_length.longLongValue;
      BOOST_REQUIRE(declared_length >= 0);
      BOOST_CHECK(std::cmp_equal(declared_length, received->body.length));
      auto expected_body = [@"<html><body>Authorization complete.<br> Return to the app.</body></html>"
          dataUsingEncoding:NSUTF8StringEncoding];
      BOOST_CHECK([received->body isEqualToData:expected_body]);
      BOOST_CHECK_EQUAL(callbacks, 1U);
      BOOST_CHECK(authorized);
      BOOST_CHECK(handler.currentAuthorizationFlow == nil);

      const auto [connected, error] = connect_to_loopback(redirect);
      BOOST_CHECK_EQUAL(connected, -1);
      BOOST_CHECK_EQUAL(error, ECONNREFUSED);
      [handler cancelHTTPListener];
      [flow cancel];
      BOOST_CHECK_EQUAL(callbacks, 1U);
   }
}

BOOST_AUTO_TEST_CASE(persisted_state_binding_rejects_immutable_config_mismatch) {
   using forge::auth::appauth::detail::auth_state_config;
   using forge::auth::appauth::detail::canonical_auth_state_binding;

   const auto configured = auth_state_config{
       .issuer = "https://issuer.example",
       .client_id = "native-client",
       .scopes = {"profile", "openid"},
       .audience = "forge-api",
   };
   const auto binding = canonical_auth_state_binding(configured);

   auto reordered_scopes = configured;
   std::swap(reordered_scopes.scopes[0], reordered_scopes.scopes[1]);
   BOOST_CHECK_EQUAL(binding, canonical_auth_state_binding(reordered_scopes));

   auto different_issuer = configured;
   different_issuer.issuer = "https://other-issuer.example";
   BOOST_CHECK_NE(binding, canonical_auth_state_binding(different_issuer));

   auto different_client = configured;
   different_client.client_id = "other-client";
   BOOST_CHECK_NE(binding, canonical_auth_state_binding(different_client));

   auto different_scopes = configured;
   different_scopes.scopes.push_back("email");
   BOOST_CHECK_NE(binding, canonical_auth_state_binding(different_scopes));

   auto different_audience = configured;
   different_audience.audience.reset();
   BOOST_CHECK_NE(binding, canonical_auth_state_binding(different_audience));
}

BOOST_AUTO_TEST_CASE(invalidation_rejects_stale_state_mutation) {
   auto epoch = forge::auth::appauth::detail::auth_state_epoch{};
   const auto stale = epoch.capture();
   auto erased = false;
   auto restored = false;
   auto stale_cleanup = false;

   epoch.invalidate([&] { erased = true; });
   BOOST_CHECK(erased);
   BOOST_CHECK(!epoch.commit_if_current(stale, [&] { restored = true; }));
   BOOST_CHECK(!epoch.commit_if_current(stale, [&] { stale_cleanup = true; }));
   BOOST_CHECK(!restored);
   BOOST_CHECK(!stale_cleanup);

   const auto current = epoch.capture();
   BOOST_CHECK(epoch.commit_if_current(current, [&] { restored = true; }));
   BOOST_CHECK(restored);
   BOOST_CHECK(!epoch.commit_if_current(current, [&] { stale_cleanup = true; }));
   BOOST_CHECK(!stale_cleanup);
}

BOOST_AUTO_TEST_CASE(concurrent_state_mutations_consume_an_epoch_ticket_once) {
   auto epoch = forge::auth::appauth::detail::auth_state_epoch{};
   const auto ticket = epoch.capture();
   auto start = std::barrier{3};
   auto accepted = std::atomic_uint32_t{};
   auto actions = std::atomic_uint32_t{};

   const auto commit = [&] {
      start.arrive_and_wait();
      if (epoch.commit_if_current(ticket, [&] { actions.fetch_add(1U, std::memory_order_relaxed); })) {
         accepted.fetch_add(1U, std::memory_order_relaxed);
      }
   };
   auto first = std::thread{commit};
   auto second = std::thread{commit};
   start.arrive_and_wait();
   first.join();
   second.join();

   BOOST_CHECK_EQUAL(accepted.load(std::memory_order_relaxed), 1U);
   BOOST_CHECK_EQUAL(actions.load(std::memory_order_relaxed), 1U);

   const auto current = epoch.capture();
   BOOST_CHECK(epoch.commit_if_current(current, [&] { actions.fetch_add(1U, std::memory_order_relaxed); }));
   BOOST_CHECK_EQUAL(actions.load(std::memory_order_relaxed), 2U);
}

BOOST_AUTO_TEST_SUITE_END()
