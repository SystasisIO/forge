#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/control/if.hpp>
#include <boost/preprocessor/facilities/check_empty.hpp>
#include <boost/preprocessor/logical/not.hpp>
#include <boost/preprocessor/punctuation/comma_if.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/seq/for_each_i.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/tuple/to_seq.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

#define FORGE_CHAIN_ACTION(NAME, ...)                                                                                 \
   (ACTION, NAME, BOOST_PP_NOT(BOOST_PP_CHECK_EMPTY(__VA_ARGS__)), (__VA_ARGS__))
#define FORGE_CHAIN_READ(NAME, ...)                                                                                    \
   (READ, NAME, BOOST_PP_NOT(BOOST_PP_CHECK_EMPTY(__VA_ARGS__)), (__VA_ARGS__))

#define FORGE_CHAIN_DETAIL_KIND(METHOD) BOOST_PP_TUPLE_ELEM(4, 0, METHOD)
#define FORGE_CHAIN_DETAIL_NAME(ACTION) BOOST_PP_TUPLE_ELEM(4, 1, ACTION)
#define FORGE_CHAIN_DETAIL_HAS_FIELDS(ACTION) BOOST_PP_TUPLE_ELEM(4, 2, ACTION)
#define FORGE_CHAIN_DETAIL_FIELDS(ACTION) BOOST_PP_TUPLE_ELEM(4, 3, ACTION)
#define FORGE_CHAIN_DETAIL_METHOD(INTERFACE, ACTION) &INTERFACE::FORGE_CHAIN_DETAIL_NAME(ACTION)
#define FORGE_CHAIN_DETAIL_REQUEST(INTERFACE, ACTION)                                                                 \
   ::forge::api::chain::detail::method_request_t<FORGE_CHAIN_DETAIL_METHOD(INTERFACE, ACTION)>

#define FORGE_CHAIN_DETAIL_FOR_FIELDS_EMPTY(r, MACRO, DATA, ACTION)
#define FORGE_CHAIN_DETAIL_FOR_FIELDS_NONEMPTY(r, MACRO, DATA, ACTION)                                                  \
   BOOST_PP_SEQ_FOR_EACH_I_R(r, MACRO, DATA, BOOST_PP_TUPLE_TO_SEQ(FORGE_CHAIN_DETAIL_FIELDS(ACTION)))
#define FORGE_CHAIN_DETAIL_FOR_FIELDS(r, MACRO, DATA, ACTION)                                                          \
   BOOST_PP_IF(FORGE_CHAIN_DETAIL_HAS_FIELDS(ACTION), FORGE_CHAIN_DETAIL_FOR_FIELDS_NONEMPTY,                            \
               FORGE_CHAIN_DETAIL_FOR_FIELDS_EMPTY)(r, MACRO, DATA, ACTION)

#define FORGE_CHAIN_DETAIL_FIELD_TYPE(REQUEST, FIELD) std::remove_cvref_t<decltype(REQUEST::FIELD)>
#define FORGE_CHAIN_DETAIL_FIELD_PARAMETER(r, REQUEST, i, FIELD)                                                       \
   BOOST_PP_COMMA_IF(i) FORGE_CHAIN_DETAIL_FIELD_TYPE(REQUEST, FIELD) FIELD
#define FORGE_CHAIN_DETAIL_FIELD_TUPLE_TYPE(r, REQUEST, i, FIELD)                                                      \
   BOOST_PP_COMMA_IF(i) FORGE_CHAIN_DETAIL_FIELD_TYPE(REQUEST, FIELD)
#define FORGE_CHAIN_DETAIL_FIELD_MOVE(r, _, i, FIELD) BOOST_PP_COMMA_IF(i) std::move(FIELD)
#define FORGE_CHAIN_DETAIL_FIELD_INIT(r, _, i, FIELD) .FIELD = std::move(std::get<i>(forge_api_chain_fields)),

#define FORGE_CHAIN_DETAIL_DESCRIPTOR(r, INTERFACE, ACTION)                                                           \
   ::forge::api::core::method_descriptor{                                                                             \
      .name = BOOST_PP_STRINGIZE(FORGE_CHAIN_DETAIL_NAME(ACTION)),                                                    \
      .response_type = typeid(::forge::api::chain::detail::method_result_t<                                            \
         FORGE_CHAIN_DETAIL_METHOD(INTERFACE, ACTION)>),                                                               \
      .argument_names = ::forge::api::core::detail::argument_names_from_macro(                                        \
         BOOST_PP_STRINGIZE(FORGE_CHAIN_DETAIL_FIELDS(ACTION))),                                                     \
   },

#define FORGE_CHAIN_DETAIL_PROXY_ACTION(r, INTERFACE, ACTION)                                                         \
   [[nodiscard]] boost::asio::awaitable<::forge::chain::protocol::transaction_id>                                     \
   FORGE_CHAIN_DETAIL_NAME(ACTION)(                                                                                  \
       FORGE_CHAIN_DETAIL_FOR_FIELDS(r, FORGE_CHAIN_DETAIL_FIELD_PARAMETER,                                          \
                                     FORGE_CHAIN_DETAIL_REQUEST(INTERFACE, ACTION), ACTION)                            \
       BOOST_PP_COMMA_IF(FORGE_CHAIN_DETAIL_HAS_FIELDS(ACTION))                                                        \
       std::optional<::forge::api::chain::action_options> forge_api_chain_options = {}) const {                        \
      return ::forge::api::chain::detail::invoke<INTERFACE, FORGE_CHAIN_DETAIL_METHOD(INTERFACE, ACTION)>(             \
         this->client_, this->implementation_, BOOST_PP_STRINGIZE(FORGE_CHAIN_DETAIL_NAME(ACTION)),                   \
         forge_api_chain_options,                                                                                      \
         std::tuple<FORGE_CHAIN_DETAIL_FOR_FIELDS(r, FORGE_CHAIN_DETAIL_FIELD_TUPLE_TYPE,                              \
                                                 FORGE_CHAIN_DETAIL_REQUEST(INTERFACE, ACTION), ACTION)>{            \
            FORGE_CHAIN_DETAIL_FOR_FIELDS(r, FORGE_CHAIN_DETAIL_FIELD_MOVE, _, ACTION)},                             \
         []([[maybe_unused]] auto& forge_api_chain_fields,                                                            \
            ::forge::chain::transaction::builder& forge_api_chain_transaction) {                                      \
            return FORGE_CHAIN_DETAIL_REQUEST(INTERFACE, ACTION){                                                    \
               FORGE_CHAIN_DETAIL_FOR_FIELDS(r, FORGE_CHAIN_DETAIL_FIELD_INIT, _, ACTION)                            \
               .transaction = forge_api_chain_transaction,                                                          \
            };                                                                                                       \
         });                                                                                                         \
   }

#define FORGE_CHAIN_DETAIL_PROXY_READ(r, INTERFACE, ACTION)                                                            \
   FORGE_CHAIN_DETAIL_PROXY_READ_AT(r, INTERFACE, ACTION, ::forge::chain::protocol::block_id)                          \
   FORGE_CHAIN_DETAIL_PROXY_READ_AT(r, INTERFACE, ACTION, const ::forge::api::chain::read_session&)

#define FORGE_CHAIN_DETAIL_PROXY_READ_AT(r, INTERFACE, ACTION, SOURCE)                                                 \
   [[nodiscard]] boost::asio::awaitable<::forge::api::chain::detail::method_result_t<                                  \
      FORGE_CHAIN_DETAIL_METHOD(INTERFACE, ACTION)>>                                                                   \
   FORGE_CHAIN_DETAIL_NAME(ACTION)(SOURCE forge_api_chain_anchor                                                       \
       BOOST_PP_COMMA_IF(FORGE_CHAIN_DETAIL_HAS_FIELDS(ACTION))                                                        \
       FORGE_CHAIN_DETAIL_FOR_FIELDS(r, FORGE_CHAIN_DETAIL_FIELD_PARAMETER,                                          \
                                     FORGE_CHAIN_DETAIL_REQUEST(INTERFACE, ACTION), ACTION)) const {                \
      return ::forge::api::chain::detail::read<INTERFACE, FORGE_CHAIN_DETAIL_METHOD(INTERFACE, ACTION)>(               \
         this->client_, this->implementation_, BOOST_PP_STRINGIZE(FORGE_CHAIN_DETAIL_NAME(ACTION)),                   \
         forge_api_chain_anchor,                                                                                       \
         std::tuple<FORGE_CHAIN_DETAIL_FOR_FIELDS(r, FORGE_CHAIN_DETAIL_FIELD_TUPLE_TYPE,                              \
                                                 FORGE_CHAIN_DETAIL_REQUEST(INTERFACE, ACTION), ACTION)>{            \
            FORGE_CHAIN_DETAIL_FOR_FIELDS(r, FORGE_CHAIN_DETAIL_FIELD_MOVE, _, ACTION)},                             \
         []([[maybe_unused]] auto& forge_api_chain_fields,                                                            \
            const ::forge::api::chain::read_snapshot& forge_api_chain_snapshot) {                                      \
            return FORGE_CHAIN_DETAIL_REQUEST(INTERFACE, ACTION){                                                    \
               FORGE_CHAIN_DETAIL_FOR_FIELDS(r, FORGE_CHAIN_DETAIL_FIELD_INIT, _, ACTION)                            \
               .state = forge_api_chain_snapshot,                                                                      \
            };                                                                                                       \
         });                                                                                                         \
   }

#define FORGE_CHAIN_DETAIL_PROXY(r, INTERFACE, METHOD)                                                                 \
   BOOST_PP_CAT(FORGE_CHAIN_DETAIL_PROXY_, FORGE_CHAIN_DETAIL_KIND(METHOD))(r, INTERFACE, METHOD)

#define FORGE_CHAIN_API(INTERFACE, ...)                                                                               \
   export {                                                                                                         \
   namespace forge::api::core {                                                                                     \
   template <> struct api_traits<INTERFACE> {                                                                        \
      static_assert(::forge::api::core::local_interface<INTERFACE> &&                                                \
                    INTERFACE::api_surface == ::forge::api::core::surface::local,                                    \
                    "Chain API contracts must use the local API surface");                                         \
      [[nodiscard]] static api_id id() {                                                                            \
         return ::forge::api::chain::detail::contract_id(BOOST_PP_STRINGIZE(INTERFACE));                               \
      }                                                                                                              \
      [[nodiscard]] static constexpr api_version version() { return {.major = 1, .revision = 0}; }                    \
      [[nodiscard]] static descriptor describe() {                                                                   \
         return {.id = id(), .version = version(), .supported_surfaces = surface::local,                              \
                 .interface_type = typeid(INTERFACE),                                                               \
                 .methods = {BOOST_PP_SEQ_FOR_EACH(FORGE_CHAIN_DETAIL_DESCRIPTOR, INTERFACE,                           \
                                                   BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))}};                         \
      }                                                                                                              \
   };                                                                                                                \
   }                                                                                                                 \
   namespace forge::api::chain {                                                                                     \
   template <> class proxy<INTERFACE> {                                                                              \
    public:                                                                                                          \
      proxy(std::shared_ptr<const ::forge::api::chain::detail::client> client,                                        \
            ::forge::api::core::handle<INTERFACE> implementation)                                                    \
          : client_{std::move(client)}, implementation_{std::move(implementation)} {}                                \
      BOOST_PP_SEQ_FOR_EACH(FORGE_CHAIN_DETAIL_PROXY, INTERFACE, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))                \
    private:                                                                                                         \
      std::shared_ptr<const ::forge::api::chain::detail::client> client_;                                             \
      ::forge::api::core::handle<INTERFACE> implementation_;                                                          \
   };                                                                                                                \
   }                                                                                                                 \
   }
