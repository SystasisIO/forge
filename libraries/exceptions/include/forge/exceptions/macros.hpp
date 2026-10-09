#pragma once

#include <chrono>
#include <source_location>
#include <string>
#include <system_error>
#include <type_traits>

#ifndef __func__
#define __func__ __FUNCTION__
#endif

#if __APPLE__
#define LIKELY(x) __builtin_expect((long)!!(x), 1L)
#define UNLIKELY(x) __builtin_expect((long)!!(x), 0L)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#ifndef FORGE_MULTILINE_MACRO_BEGIN
#define FORGE_MULTILINE_MACRO_BEGIN do {
#ifdef _MSC_VER
#define FORGE_MULTILINE_MACRO_END                                                                                        \
   __pragma(warning(push)) __pragma(warning(disable : 4127))                                                           \
   }                                                                                                                   \
   while (0)                                                                                                           \
   __pragma(warning(pop))
#else
#define FORGE_MULTILINE_MACRO_END                                                                                        \
   }                                                                                                                   \
   while (0)
#endif
#endif

#define FORGE_THROW(MESSAGE, ...)                                                                                        \
   FORGE_MULTILINE_MACRO_BEGIN                                                                                           \
   forge::exceptions::throw_with_context(MESSAGE, std::source_location::current() __VA_OPT__(, ) __VA_ARGS__);            \
   FORGE_MULTILINE_MACRO_END

#define FORGE_DECLARE_EXCEPTION_CATEGORY(Enum, CATEGORY_NAME)                                                            \
   inline const std::error_category& forge_exceptions_category(Enum) noexcept {                                           \
      static const forge::exceptions::category instance{CATEGORY_NAME};                                                    \
      return instance;                                                                                                 \
   }                                                                                                                   \
   inline std::error_code make_error_code(Enum value) noexcept {                                                       \
      return std::error_code{static_cast<int>(value), forge_exceptions_category(value)};                                   \
   }

#define FORGE_THROW_EXCEPTION(ExceptionType, MESSAGE, ...)                                                               \
   FORGE_MULTILINE_MACRO_BEGIN                                                                                           \
   static_assert(std::is_base_of_v<forge::exceptions::base, ExceptionType>,                                               \
                 "FORGE_THROW_EXCEPTION expects a type derived from forge::exceptions::base");                             \
   throw ExceptionType(std::string(MESSAGE),                                                                           \
                       forge::exceptions::make_fields(__VA_ARGS__),                                                       \
                       std::source_location::current());                                                               \
   FORGE_MULTILINE_MACRO_END

#define FORGE_THROW_CODE(CodeValue, MESSAGE, ...)                                                                        \
   FORGE_MULTILINE_MACRO_BEGIN                                                                                           \
   const auto forge_exceptions_code_value = (CodeValue);                                                                  \
   forge::exceptions::throw_code(forge_exceptions_code_value,                                                                 \
                              std::string(MESSAGE),                                                                    \
                              forge::exceptions::make_fields(__VA_ARGS__),                                                \
                              std::source_location::current());                                                        \
   FORGE_MULTILINE_MACRO_END

// Typed operational data belongs to the concrete exception, not diagnostic fields.
// Its constructor accepts (data, message, fields, source_location).
#define FORGE_THROW_EXCEPTION_WITH_DATA(ExceptionType, DATA, MESSAGE, ...)                                                \
   FORGE_MULTILINE_MACRO_BEGIN                                                                                           \
   static_assert(std::is_base_of_v<forge::exceptions::base, ExceptionType>,                                               \
                 "FORGE_THROW_EXCEPTION_WITH_DATA expects a type derived from forge::exceptions::base");                 \
   throw ExceptionType((DATA), std::string(MESSAGE),                                                                     \
                       forge::exceptions::make_fields(__VA_ARGS__),                                                     \
                       std::source_location::current());                                                               \
   FORGE_MULTILINE_MACRO_END

#define FORGE_ASSERT(TEST, ...)                                                                                          \
   FORGE_MULTILINE_MACRO_BEGIN                                                                                           \
   if (UNLIKELY(!(TEST))) {                                                                                            \
      forge::exceptions::throw_assertion_failure(#TEST, std::source_location::current() __VA_OPT__(, ) __VA_ARGS__);      \
   }                                                                                                                   \
   FORGE_MULTILINE_MACRO_END

#define FORGE_CHECK_DEADLINE(DEADLINE, ...)                                                                              \
   FORGE_MULTILINE_MACRO_BEGIN                                                                                           \
   const auto forge_deadline_value = (DEADLINE);                                                                         \
   if (forge_deadline_value < decltype(forge_deadline_value)::max() &&                                                     \
       forge_deadline_value < decltype(forge_deadline_value)::clock::now()) {                                              \
      forge::exceptions::throw_deadline_exceeded("deadline exceeded",                                                     \
                                              std::source_location::current() __VA_OPT__(, ) __VA_ARGS__);             \
   }                                                                                                                   \
   FORGE_MULTILINE_MACRO_END

#define FORGE_CAPTURE_AND_RETHROW(MESSAGE, ...)                                                                          \
   catch (...) {                                                                                                       \
      forge::exceptions::capture_and_rethrow(MESSAGE, std::source_location::current() __VA_OPT__(, ) __VA_ARGS__);        \
   }

#define FORGE_CAPTURE_LOG_AND_RETHROW(MESSAGE, ...)                                                                      \
   catch (...) {                                                                                                       \
      forge::exceptions::capture_and_log(MESSAGE __VA_OPT__(, ) __VA_ARGS__);                                             \
      forge::exceptions::capture_and_rethrow(MESSAGE, std::source_location::current() __VA_OPT__(, ) __VA_ARGS__);        \
   }

#define FORGE_CAPTURE_AND_LOG(MESSAGE, ...)                                                                              \
   catch (...) {                                                                                                       \
      forge::exceptions::capture_and_log(MESSAGE __VA_OPT__(, ) __VA_ARGS__);                                             \
   }

#define FORGE_LOG_AND_RETHROW() FORGE_CAPTURE_AND_RETHROW("rethrow")
#define FORGE_LOG_AND_DROP(...) FORGE_CAPTURE_AND_LOG("drop" __VA_OPT__(, ) __VA_ARGS__)
