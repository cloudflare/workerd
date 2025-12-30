// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/exception.h>

namespace workerd {

// If an exception is thrown for exceeding memory limits, it will contain this detail.
constexpr kj::Exception::DetailTypeId MEMORY_LIMIT_DETAIL_ID = 0xbaf76dd7ce5bd8cfull;

}  // namespace workerd

// KJ exception handling macros that allow catching exceptions by type instead of using
// the awkward `catch (...)` + `kj::getCaughtExceptionAsKj()` pattern.
//
// Usage:
//   KJ_TRY {
//     someCode();
//   } KJ_CATCH (kj::Exception& exception) {
//     handleException(exception);
//   }
//
// This expands to a nested try-catch where the inner catch converts any exception
// to kj::Exception using getCaughtExceptionAsKj(), then re-throws it so the outer
// catch can handle it by specific type.
#define KJ_TRY                                                                                     \
  try {                                                                                            \
    try
#define KJ_CATCH                                                                                   \
  catch (...) {                                                                                    \
    throw kj::getCaughtExceptionAsKj();                                                            \
  }                                                                                                \
  }                                                                                                \
  catch
