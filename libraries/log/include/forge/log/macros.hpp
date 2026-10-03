#pragma once

#include <source_location>

#define FORGE_DETAIL_LOG_SELECT(_1, _2, _3, NAME, ...) NAME
#define FORGE_DETAIL_LOG_JOIN_I(A, B) A##B
#define FORGE_DETAIL_LOG_JOIN(A, B) FORGE_DETAIL_LOG_JOIN_I(A, B)
#define FORGE_DETAIL_LOG_DEFAULT(LEVEL, MESSAGE) FORGE_DETAIL_LOG_NAMED(LEVEL, "default", MESSAGE)
#define FORGE_DETAIL_LOG_NAMED(LEVEL, LOGGER, MESSAGE)                                                                 \
   FORGE_DETAIL_LOG_NAMED_I(LEVEL, LOGGER, MESSAGE, FORGE_DETAIL_LOG_JOIN(forge_log_route_, __COUNTER__))
#define FORGE_DETAIL_LOG_NAMED_I(LEVEL, LOGGER, MESSAGE, ROUTE)                                                        \
   do {                                                                                                                \
      auto ROUTE = ::forge::resolve_log_logger((LOGGER));                                                              \
      if (ROUTE.is_enabled(::forge::log_level::LEVEL)) {                                                               \
         ::forge::emit_log(ROUTE, ::forge::log_level::LEVEL, (MESSAGE), ::std::source_location::current());            \
      }                                                                                                                \
   } while (0)
#define FORGE_DETAIL_LOG_FIELDS(LEVEL, LOGGER, MESSAGE, FIELDS)                                                        \
   FORGE_DETAIL_LOG_FIELDS_I(LEVEL, LOGGER, MESSAGE, FIELDS, FORGE_DETAIL_LOG_JOIN(forge_log_route_, __COUNTER__))
#define FORGE_DETAIL_LOG_FIELDS_I(LEVEL, LOGGER, MESSAGE, FIELDS, ROUTE)                                               \
   do {                                                                                                                \
      auto ROUTE = ::forge::resolve_log_logger((LOGGER));                                                              \
      if (ROUTE.is_enabled(::forge::log_level::LEVEL)) {                                                               \
         ::forge::emit_log(ROUTE, ::forge::log_level::LEVEL, (MESSAGE), (::forge::log_field_builder {} FIELDS).take(), \
                           ::std::source_location::current());                                                         \
      }                                                                                                                \
   } while (0)
#define FORGE_DETAIL_LOG_DISPATCH(LEVEL, ...)                                                                          \
   FORGE_DETAIL_LOG_SELECT(__VA_ARGS__, FORGE_DETAIL_LOG_FIELDS, FORGE_DETAIL_LOG_NAMED,                               \
                           FORGE_DETAIL_LOG_DEFAULT)(LEVEL, __VA_ARGS__)

#ifdef FORGE_DISABLE_LOGGING
#define tlog(...)                                                                                                      \
   do {                                                                                                                \
   } while (0)
#define dlog(...)                                                                                                      \
   do {                                                                                                                \
   } while (0)
#define ilog(...)                                                                                                      \
   do {                                                                                                                \
   } while (0)
#define wlog(...)                                                                                                      \
   do {                                                                                                                \
   } while (0)
#define elog(...)                                                                                                      \
   do {                                                                                                                \
   } while (0)
#else
#define tlog(...) FORGE_DETAIL_LOG_DISPATCH(all, __VA_ARGS__)
#define dlog(...) FORGE_DETAIL_LOG_DISPATCH(debug, __VA_ARGS__)
#define ilog(...) FORGE_DETAIL_LOG_DISPATCH(info, __VA_ARGS__)
#define wlog(...) FORGE_DETAIL_LOG_DISPATCH(warn, __VA_ARGS__)
#define elog(...) FORGE_DETAIL_LOG_DISPATCH(error, __VA_ARGS__)
#endif
