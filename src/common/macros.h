#pragma once

#include <exception>
#include <iostream>

/// Branch prediction hints.
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/// Check condition and terminate if not true.
///
/// This must stay a macro, not an inline function: the message argument is
/// only evaluated on the failure path, so the success path performs no string
/// formatting and no allocation. The reference book implementation used an
/// inline function, which evaluated and formatted the message at every call
/// site and therefore allocated on the hot path.
#define ASSERT(condition, message)                                             \
  do {                                                                         \
    if (UNLIKELY(!(condition))) {                                              \
      std::cerr << "ASSERT : " << (message) << std::endl;                      \
      std::terminate();                                                        \
    }                                                                          \
  } while (false)

/// Report a fatal condition and terminate.
#define FATAL(message)                                                         \
  do {                                                                         \
    std::cerr << "FATAL : " << (message) << std::endl;                         \
    std::terminate();                                                          \
  } while (false)
