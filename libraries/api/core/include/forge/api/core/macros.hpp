#pragma once

#include <boost/preprocessor/control/if.hpp>
#include <boost/preprocessor/facilities/check_empty.hpp>
#include <boost/preprocessor/logical/not.hpp>
#include <boost/preprocessor/punctuation/comma_if.hpp>
#include <boost/preprocessor/punctuation/remove_parens.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/seq/for_each_i.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/tuple/size.hpp>
#include <boost/preprocessor/tuple/to_seq.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define FORGE_API_CONTRACT(ID, MAJOR, REVISION) (ID, MAJOR, REVISION)

#define FORGE_API_DETAIL_HAS_ARGS(...) BOOST_PP_NOT(BOOST_PP_CHECK_EMPTY(__VA_ARGS__))

#define FORGE_API_METHOD(NAME, ...)                                                                                    \
   (NAME, void, void, 0, 0, "", 0, FORGE_API_DETAIL_HAS_ARGS(__VA_ARGS__), (__VA_ARGS__), 0, ())
#define FORGE_API_METHOD_EXACT(NAME, POINTER, ...)                                                                     \
   (NAME, void, void, 0, 0, "", 0, FORGE_API_DETAIL_HAS_ARGS(__VA_ARGS__), (__VA_ARGS__), 1, (POINTER))
#define FORGE_API_METHOD_SINCE(NAME, REVISION, ...)                                                                    \
   (NAME, void, void, REVISION, 0, "", 0, FORGE_API_DETAIL_HAS_ARGS(__VA_ARGS__), (__VA_ARGS__), 0, ())
#define FORGE_API_METHOD_DEPRECATED(NAME, REASON, ...)                                                                 \
   (NAME, void, void, 0, 1, REASON, 0, FORGE_API_DETAIL_HAS_ARGS(__VA_ARGS__), (__VA_ARGS__), 0, ())
#define FORGE_API_METHOD_DEPRECATED_SINCE(NAME, REVISION, REASON, ...)                                                 \
   (NAME, void, void, REVISION, 1, REASON, 0, FORGE_API_DETAIL_HAS_ARGS(__VA_ARGS__), (__VA_ARGS__), 0, ())
#define FORGE_API_METHOD_TYPED(NAME, REQUEST, RESPONSE) (NAME, REQUEST, RESPONSE, 0, 0, "", 1, 0, (), 0, ())
#define FORGE_API_METHOD_TYPED_SINCE(NAME, REQUEST, RESPONSE, REVISION)                                                \
   (NAME, REQUEST, RESPONSE, REVISION, 0, "", 1, 0, (), 0, ())
#define FORGE_API_METHOD_TYPED_DEPRECATED(NAME, REQUEST, RESPONSE, REASON)                                             \
   (NAME, REQUEST, RESPONSE, 0, 1, REASON, 1, 0, (), 0, ())
#define FORGE_API_METHOD_TYPED_DEPRECATED_SINCE(NAME, REQUEST, RESPONSE, REVISION, REASON)                             \
   (NAME, REQUEST, RESPONSE, REVISION, 1, REASON, 1, 0, (), 0, ())

#if defined(__clang__)
#define FORGE_API_DETAIL_DIAGNOSTIC_PUSH                                                                               \
   _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define FORGE_API_DETAIL_DIAGNOSTIC_POP _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define FORGE_API_DETAIL_DIAGNOSTIC_PUSH                                                                               \
   _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define FORGE_API_DETAIL_DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")
#else
#define FORGE_API_DETAIL_DIAGNOSTIC_PUSH
#define FORGE_API_DETAIL_DIAGNOSTIC_POP
#endif

#define FORGE_API_DETAIL_METHOD_NAME(METHOD) BOOST_PP_TUPLE_ELEM(11, 0, METHOD)
#define FORGE_API_DETAIL_METHOD_REQUEST(METHOD) BOOST_PP_TUPLE_ELEM(11, 1, METHOD)
#define FORGE_API_DETAIL_METHOD_RESPONSE(METHOD) BOOST_PP_TUPLE_ELEM(11, 2, METHOD)
#define FORGE_API_DETAIL_METHOD_SINCE(METHOD) BOOST_PP_TUPLE_ELEM(11, 3, METHOD)
#define FORGE_API_DETAIL_METHOD_DEPRECATED(METHOD) BOOST_PP_TUPLE_ELEM(11, 4, METHOD)
#define FORGE_API_DETAIL_METHOD_REASON(METHOD) BOOST_PP_TUPLE_ELEM(11, 5, METHOD)
#define FORGE_API_DETAIL_METHOD_TYPED(METHOD) BOOST_PP_TUPLE_ELEM(11, 6, METHOD)
#define FORGE_API_DETAIL_METHOD_HAS_ARGS(METHOD) BOOST_PP_TUPLE_ELEM(11, 7, METHOD)
#define FORGE_API_DETAIL_METHOD_ARGS(METHOD) BOOST_PP_TUPLE_ELEM(11, 8, METHOD)
#define FORGE_API_DETAIL_METHOD_EXACT(METHOD) BOOST_PP_TUPLE_ELEM(11, 9, METHOD)
#define FORGE_API_DETAIL_METHOD_EXACT_POINTER(METHOD) BOOST_PP_TUPLE_ELEM(11, 10, METHOD)

#define FORGE_API_DETAIL_METHOD_POINTER_DEDUCED(INTERFACE, METHOD) &INTERFACE::FORGE_API_DETAIL_METHOD_NAME(METHOD)
#define FORGE_API_DETAIL_METHOD_POINTER_EXACT(INTERFACE, METHOD)                                                       \
   BOOST_PP_REMOVE_PARENS(FORGE_API_DETAIL_METHOD_EXACT_POINTER(METHOD))
#define FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)                                                             \
   BOOST_PP_IF(FORGE_API_DETAIL_METHOD_EXACT(METHOD), FORGE_API_DETAIL_METHOD_POINTER_EXACT,                           \
               FORGE_API_DETAIL_METHOD_POINTER_DEDUCED)(INTERFACE, METHOD)

#define FORGE_API_DETAIL_ARG_NAMES_EMPTY_R(r, METHOD)                                                                  \
   std::vector<std::string> {}
#define FORGE_API_DETAIL_ARG_NAMES_NONEMPTY_R(r, METHOD)                                                               \
   ::forge::api::core::detail::argument_names_from_macro(BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_ARGS(METHOD)))
#define FORGE_API_DETAIL_ARG_NAMES_R(r, METHOD)                                                                        \
   BOOST_PP_IF(FORGE_API_DETAIL_METHOD_HAS_ARGS(METHOD), FORGE_API_DETAIL_ARG_NAMES_NONEMPTY_R,                        \
               FORGE_API_DETAIL_ARG_NAMES_EMPTY_R)(r, METHOD)

#define FORGE_API_DETAIL_TYPED_METHOD_POINTER(INTERFACE, METHOD)                                                       \
   static_cast<boost::asio::awaitable<FORGE_API_DETAIL_METHOD_RESPONSE(METHOD)> (INTERFACE::*)(                        \
       FORGE_API_DETAIL_METHOD_REQUEST(METHOD))>(&INTERFACE::FORGE_API_DETAIL_METHOD_NAME(METHOD))

#define FORGE_API_DETAIL_DESCRIPTOR_METHODS(INTERFACE, ...)                                                            \
   __VA_OPT__(                                                                                                         \
       BOOST_PP_SEQ_FOR_EACH(FORGE_API_DETAIL_DESCRIPTOR_METHOD, INTERFACE, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)))

#define FORGE_API_DETAIL_PROXY_METHODS(INTERFACE, ...)                                                                 \
   __VA_OPT__(BOOST_PP_SEQ_FOR_EACH(FORGE_API_DETAIL_PROXY_METHOD, INTERFACE, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)))

#define FORGE_API_DETAIL_DESCRIPTOR_METHOD(r, INTERFACE, METHOD)                                                       \
   BOOST_PP_IF(FORGE_API_DETAIL_METHOD_TYPED(METHOD), FORGE_API_DETAIL_DESCRIPTOR_METHOD_TYPED,                        \
               FORGE_API_DETAIL_DESCRIPTOR_METHOD_DEDUCED)(r, INTERFACE, METHOD)

#define FORGE_API_DETAIL_DESCRIPTOR_METHOD_DEDUCED(r, INTERFACE, METHOD)                                               \
   {                                                                                                                   \
      auto method = builder.template method<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                       \
          BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD)), FORGE_API_DETAIL_ARG_NAMES_R(r, METHOD),           \
          server_fields<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>());                                       \
      method.since_revision(FORGE_API_DETAIL_METHOD_SINCE(METHOD));                                                    \
      BOOST_PP_IF(                                                                                                     \
          FORGE_API_DETAIL_METHOD_DEPRECATED(METHOD), method.deprecated(FORGE_API_DETAIL_METHOD_REASON(METHOD));, )    \
      ::forge::api::core::customize_method_descriptor<INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(  \
          method);                                                                                                     \
   }
#define FORGE_API_DETAIL_DESCRIPTOR_METHOD_TYPED(r, INTERFACE, METHOD)                                                 \
   {                                                                                                                   \
      auto method =                                                                                                    \
          builder.template method<FORGE_API_DETAIL_TYPED_METHOD_POINTER(INTERFACE, METHOD),                            \
                                  FORGE_API_DETAIL_METHOD_REQUEST(METHOD), FORGE_API_DETAIL_METHOD_RESPONSE(METHOD)>(  \
              BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD)),                                                \
              server_fields<FORGE_API_DETAIL_TYPED_METHOD_POINTER(INTERFACE, METHOD)>());                             \
      method.since_revision(FORGE_API_DETAIL_METHOD_SINCE(METHOD));                                                    \
      BOOST_PP_IF(                                                                                                     \
          FORGE_API_DETAIL_METHOD_DEPRECATED(METHOD), method.deprecated(FORGE_API_DETAIL_METHOD_REASON(METHOD));, )    \
      ::forge::api::core::customize_method_descriptor<INTERFACE, FORGE_API_DETAIL_TYPED_METHOD_POINTER(                \
                                                                     INTERFACE, METHOD)>(method);                      \
   }

#define FORGE_API_DETAIL_PROXY_METHOD(r, INTERFACE, METHOD)                                                            \
   BOOST_PP_IF(FORGE_API_DETAIL_METHOD_TYPED(METHOD), FORGE_API_DETAIL_PROXY_METHOD_TYPED,                             \
               FORGE_API_DETAIL_PROXY_METHOD_DEDUCED)(r, INTERFACE, METHOD)

#define FORGE_API_DETAIL_PROXY_METHOD_DEDUCED(r, INTERFACE, METHOD)                                                    \
   BOOST_PP_IF(FORGE_API_DETAIL_METHOD_HAS_ARGS(METHOD), FORGE_API_DETAIL_PROXY_METHOD_POSITIONAL,                     \
               FORGE_API_DETAIL_PROXY_METHOD_ONE_REQUEST)(r, INTERFACE, METHOD)

#define FORGE_API_DETAIL_PROXY_METHOD_ONE_REQUEST(r, INTERFACE, METHOD)                                                \
   boost::asio::awaitable<::forge::api::core::method_response_t<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>>   \
   FORGE_API_DETAIL_METHOD_NAME(METHOD)() {                                                                            \
      if constexpr (::forge::api::core::method_argument_count_v<                                                      \
                        FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)> == 0) {                                    \
         co_return co_await ::forge::api::core::detail::proxy_method<                                                 \
             INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                                          \
             this->invoker_, this->selected_api_,                                                                     \
             BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD)));                                               \
      } else {                                                                                                         \
         throw ::forge::api::core::exceptions::protocol_error{"invalid generated API proxy overload"};              \
      }                                                                                                                \
   }                                                                                                                   \
   boost::asio::awaitable<::forge::api::core::method_response_t<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>>   \
   FORGE_API_DETAIL_METHOD_NAME(METHOD)() const {                                                                      \
      if constexpr (::forge::api::core::method_argument_count_v<                                                      \
                        FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)> == 0) {                                    \
         co_return co_await ::forge::api::core::detail::proxy_method<                                                 \
             INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                                          \
             this->invoker_, this->selected_api_,                                                                     \
             BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD)));                                               \
      } else {                                                                                                         \
         throw ::forge::api::core::exceptions::protocol_error{"invalid generated API proxy overload"};              \
      }                                                                                                                \
   }                                                                                                                   \
   boost::asio::awaitable<::forge::api::core::method_response_t<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>>   \
   FORGE_API_DETAIL_METHOD_NAME(METHOD)(                                                                               \
       ::forge::api::core::detail::method_argument_or_t<                                                              \
          FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD), 0> request) {                                            \
      if constexpr (::forge::api::core::method_argument_count_v<                                                      \
                        FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)> == 1) {                                    \
         co_return co_await ::forge::api::core::detail::proxy_method<                                                 \
             INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                                          \
             this->invoker_, this->selected_api_,                                                                     \
             BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD)), std::move(request));                           \
      } else {                                                                                                         \
         throw ::forge::api::core::exceptions::protocol_error{"invalid generated API proxy overload"};              \
      }                                                                                                                \
   }                                                                                                                   \
   boost::asio::awaitable<::forge::api::core::method_response_t<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>>   \
   FORGE_API_DETAIL_METHOD_NAME(METHOD)(                                                                               \
       ::forge::api::core::detail::method_argument_or_t<                                                              \
          FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD), 0> request) const {                                      \
      if constexpr (::forge::api::core::method_argument_count_v<                                                      \
                        FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)> == 1) {                                    \
         co_return co_await ::forge::api::core::detail::proxy_method<                                                 \
             INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                                          \
             this->invoker_, this->selected_api_,                                                                     \
             BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD)), std::move(request));                           \
      } else {                                                                                                         \
         throw ::forge::api::core::exceptions::protocol_error{"invalid generated API proxy overload"};              \
      }                                                                                                                \
   }

#define FORGE_API_DETAIL_PROXY_ARG_DECL(r, DATA, INDEX, ARG)                                                           \
   BOOST_PP_COMMA_IF(INDEX)                                                                                            \
   ::forge::api::core::method_argument_t<                                                                              \
       FORGE_API_DETAIL_METHOD_POINTER(BOOST_PP_TUPLE_ELEM(2, 0, DATA), BOOST_PP_TUPLE_ELEM(2, 1, DATA)), INDEX>       \
       ARG
#define FORGE_API_DETAIL_PROXY_ARG_MOVE(r, DATA, INDEX, ARG) , std::move(ARG)
#define FORGE_API_DETAIL_PROXY_ARG_DECLS_R(r, INTERFACE, METHOD)                                                       \
   BOOST_PP_SEQ_FOR_EACH_I_R(r, FORGE_API_DETAIL_PROXY_ARG_DECL, (INTERFACE, METHOD),                                  \
                             BOOST_PP_TUPLE_TO_SEQ(FORGE_API_DETAIL_METHOD_ARGS(METHOD)))
#define FORGE_API_DETAIL_PROXY_ARG_MOVES_R(r, METHOD)                                                                  \
   BOOST_PP_SEQ_FOR_EACH_I_R(r, FORGE_API_DETAIL_PROXY_ARG_MOVE, _,                                                    \
                             BOOST_PP_TUPLE_TO_SEQ(FORGE_API_DETAIL_METHOD_ARGS(METHOD)))

#define FORGE_API_DETAIL_PROXY_METHOD_POSITIONAL(r, INTERFACE, METHOD)                                                 \
   boost::asio::awaitable<::forge::api::core::method_response_t<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>>   \
   FORGE_API_DETAIL_METHOD_NAME(METHOD)(FORGE_API_DETAIL_PROXY_ARG_DECLS_R(r, INTERFACE, METHOD)) {                    \
      if constexpr (::forge::api::core::method_kind_v<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)> ==          \
                    ::forge::api::core::method_kind::unary) {                                                         \
         co_return co_await ::forge::api::core::detail::proxy_method<                                                 \
             INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                                          \
             this->invoker_, this->selected_api_,                                                                     \
             BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD))                                                 \
                FORGE_API_DETAIL_PROXY_ARG_MOVES_R(r, METHOD));                                                       \
      } else {                                                                                                         \
         throw ::forge::api::core::exceptions::protocol_error{"invalid generated API proxy overload"};              \
      }                                                                                                                \
   }                                                                                                                   \
   boost::asio::awaitable<::forge::api::core::method_response_t<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>>   \
   FORGE_API_DETAIL_METHOD_NAME(METHOD)(FORGE_API_DETAIL_PROXY_ARG_DECLS_R(r, INTERFACE, METHOD)) const {              \
      if constexpr (::forge::api::core::method_kind_v<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)> ==          \
                    ::forge::api::core::method_kind::unary) {                                                         \
         co_return co_await ::forge::api::core::detail::proxy_method<                                                 \
             INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                                          \
             this->invoker_, this->selected_api_,                                                                     \
             BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD))                                                 \
                FORGE_API_DETAIL_PROXY_ARG_MOVES_R(r, METHOD));                                                       \
      } else {                                                                                                         \
         throw ::forge::api::core::exceptions::protocol_error{"invalid generated API proxy overload"};              \
      }                                                                                                                \
   }                                                                                                                   \
   boost::asio::awaitable<::forge::api::core::method_response_t<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>>   \
   FORGE_API_DETAIL_METHOD_NAME(METHOD)(FORGE_API_DETAIL_PROXY_ARG_DECLS_R(r, INTERFACE, METHOD),                     \
       ::forge::api::core::detail::method_argument_or_t<                                                              \
          FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD),                                                         \
          BOOST_PP_TUPLE_SIZE(FORGE_API_DETAIL_METHOD_ARGS(METHOD))> forge_api_stream_endpoint) {                     \
      if constexpr (::forge::api::core::method_kind_v<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)> !=          \
                    ::forge::api::core::method_kind::unary) {                                                         \
         co_return co_await ::forge::api::core::detail::proxy_method<                                                 \
             INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                                          \
             this->invoker_, this->selected_api_,                                                                     \
             BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD))                                                 \
                FORGE_API_DETAIL_PROXY_ARG_MOVES_R(r, METHOD), std::move(forge_api_stream_endpoint));                 \
      } else {                                                                                                         \
         throw ::forge::api::core::exceptions::protocol_error{"invalid generated API proxy overload"};              \
      }                                                                                                                \
   }                                                                                                                   \
   boost::asio::awaitable<::forge::api::core::method_response_t<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>>   \
   FORGE_API_DETAIL_METHOD_NAME(METHOD)(FORGE_API_DETAIL_PROXY_ARG_DECLS_R(r, INTERFACE, METHOD),                     \
       ::forge::api::core::detail::method_argument_or_t<                                                              \
          FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD),                                                         \
          BOOST_PP_TUPLE_SIZE(FORGE_API_DETAIL_METHOD_ARGS(METHOD))> forge_api_stream_endpoint) const {               \
      if constexpr (::forge::api::core::method_kind_v<FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)> !=          \
                    ::forge::api::core::method_kind::unary) {                                                         \
         co_return co_await ::forge::api::core::detail::proxy_method<                                                 \
             INTERFACE, FORGE_API_DETAIL_METHOD_POINTER(INTERFACE, METHOD)>(                                          \
             this->invoker_, this->selected_api_,                                                                     \
             BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD))                                                 \
                FORGE_API_DETAIL_PROXY_ARG_MOVES_R(r, METHOD), std::move(forge_api_stream_endpoint));                 \
      } else {                                                                                                         \
         throw ::forge::api::core::exceptions::protocol_error{"invalid generated API proxy overload"};              \
      }                                                                                                                \
   }

#define FORGE_API_DETAIL_PROXY_METHOD_TYPED(r, INTERFACE, METHOD)                                                      \
   boost::asio::awaitable<FORGE_API_DETAIL_METHOD_RESPONSE(METHOD)> FORGE_API_DETAIL_METHOD_NAME(METHOD)(              \
       FORGE_API_DETAIL_METHOD_REQUEST(METHOD) request) override {                                                     \
      co_return co_await ::forge::api::core::detail::proxy_method<INTERFACE, FORGE_API_DETAIL_TYPED_METHOD_POINTER(    \
                                                                                 INTERFACE, METHOD)>(                  \
          this->invoker_, this->selected_api_,                                                                         \
          BOOST_PP_STRINGIZE(FORGE_API_DETAIL_METHOD_NAME(METHOD)), std::move(request));                               \
   }

#define FORGE_API(INTERFACE, CONTRACT, ...)                                                                            \
   FORGE_API_DETAIL_DIAGNOSTIC_PUSH                                                                                    \
   namespace forge::api::core {                                                                                        \
   template <> struct api_traits<INTERFACE> {                                                                          \
      static api_id id() {                                                                                             \
         return api_id{.value = BOOST_PP_TUPLE_ELEM(3, 0, CONTRACT)};                                                  \
      }                                                                                                                \
      static api_version version() {                                                                                   \
         return api_version{.major = BOOST_PP_TUPLE_ELEM(3, 1, CONTRACT),                                              \
                            .revision = BOOST_PP_TUPLE_ELEM(3, 2, CONTRACT)};                                          \
      }                                                                                                                \
      static api_ref ref(std::uint16_t min_revision = version().revision) {                                            \
         const auto value = version();                                                                                 \
         return api_ref{.id = id(), .major = value.major, .min_revision = min_revision};                               \
      }                                                                                                                \
      template <auto Method> static ::forge::api::core::detail::server_field_operations server_fields() {            \
         using wire_type = ::forge::api::core::detail::method_fixed_request_t<Method>;                               \
         using fixed_type = ::forge::api::core::detail::method_fixed_argument_tuple_t<Method>;                       \
         return {                                                                                                      \
            .reset_wire = [](void* value) {                                                                           \
               ::forge::api::core::reset_server_supplied(*static_cast<wire_type*>(value));                            \
            },                                                                                                         \
            .apply_wire = [](void* value, const ::forge::api::core::trusted_invocation& trusted) {                   \
               ::forge::api::core::apply_server_supplied(*static_cast<wire_type*>(value), trusted);                  \
            },                                                                                                         \
            .reset_fixed = [](void* value) {                                                                          \
               ::forge::api::core::reset_server_supplied(*static_cast<fixed_type*>(value));                           \
            },                                                                                                         \
            .apply_fixed = [](void* value, const ::forge::api::core::trusted_invocation& trusted) {                  \
               ::forge::api::core::apply_server_supplied(*static_cast<fixed_type*>(value), trusted);                 \
            },                                                                                                         \
         };                                                                                                            \
      }                                                                                                                \
      static descriptor describe() {                                                                                   \
         auto builder =                                                                                                \
             ::forge::api::core::define<INTERFACE, ::forge::api::core::supports(INTERFACE::api_surface,                \
                                                                                ::forge::api::core::surface::remote)>( \
                 descriptor{.id = id(),                                                                                \
                            .version = version(),                                                                      \
                            .supported_surfaces = INTERFACE::api_surface,                                              \
                            .interface_type = typeid(INTERFACE)});                                                     \
         FORGE_API_DETAIL_DESCRIPTOR_METHODS(INTERFACE, __VA_ARGS__)                                                   \
         return builder.build();                                                                                       \
      }                                                                                                                \
   };                                                                                                                  \
   namespace detail {                                                                                                  \
   template <> class proxy_impl<INTERFACE, true> : public INTERFACE {                                                  \
    public:                                                                                                            \
      explicit proxy_impl(std::shared_ptr<remote_invoker> invoker, api_ref selected_api)                               \
          : invoker_(std::move(invoker)), selected_api_(std::move(selected_api)) {}                                    \
      FORGE_API_DETAIL_PROXY_METHODS(INTERFACE, __VA_ARGS__)                                                           \
    private:                                                                                                           \
      std::shared_ptr<remote_invoker> invoker_;                                                                        \
      api_ref selected_api_;                                                                                           \
   };                                                                                                                  \
   }                                                                                                                   \
   template <> class proxy<INTERFACE> : public detail::proxy_impl<INTERFACE, remote_interface<INTERFACE>> {            \
    public:                                                                                                            \
      explicit proxy(std::shared_ptr<remote_invoker> invoker) : proxy(std::move(invoker), INTERFACE::ref()) {}         \
      explicit proxy(std::shared_ptr<remote_invoker> invoker, api_ref selected_api)                                    \
          : detail::proxy_impl<INTERFACE, remote_interface<INTERFACE>>(std::move(invoker), std::move(selected_api)) {} \
   };                                                                                                                  \
   }                                                                                                                   \
   FORGE_API_DETAIL_DIAGNOSTIC_POP

// Module interfaces need to export the generated explicit specializations.
// Keep this narrow exception instead of exposing arbitrary broad export blocks.
#define FORGE_EXPORT_API(...)                                                                                          \
   export {                                                                                                            \
      FORGE_API(__VA_ARGS__)                                                                                           \
   }
