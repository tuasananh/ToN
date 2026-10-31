#pragma once

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

namespace ton::detail {

[[noreturn]] inline void assertion_failed(
    std::string_view expr, std::string_view msg,
    const std::source_location location =
        std::source_location::current()) noexcept {
  std::cerr << "[TON_ASSERTION_FAIL] " << location.file_name() << ":"
            << location.line() << " (" << location.function_name()
            << "): " << expr;
  if (!msg.empty()) {
    std::cerr << " - " << msg;
  }
  std::cerr << std::endl;
  std::abort();
}

}  // namespace ton::detail

#if !defined(NDEBUG)

#define TON_ASSERT(expr, msg)                        \
  do {                                               \
    if (!(expr)) [[unlikely]] {                      \
      ::ton::detail::assertion_failed(#expr, (msg)); \
    }                                                \
  } while (false)

#define TON_BOUNDS_CHECK(expr, msg) TON_ASSERT((expr), (msg))

#define TON_ASSUME(expr) TON_ASSERT((expr), "Assumption violated: " #expr)

#else

#define TON_ASSERT(expr, msg) static_cast<void>(0)

#define TON_BOUNDS_CHECK(expr, msg) static_cast<void>(0)

#if defined(__has_cpp_attribute) && __has_cpp_attribute(assume)
#define TON_ASSUME(expr) [[assume(expr)]]
#else
#define TON_ASSUME(expr) static_cast<void>(0)
#endif

#endif
