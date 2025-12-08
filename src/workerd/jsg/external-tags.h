#pragma once

#include <v8-version.h>

#include <cstdint>
#include <type_traits>

// V8 14.3+ supports external pointer type tags for security.
#define V8_HAS_EXTERNAL_POINTER_TAGS                                                               \
  (V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 3))

namespace workerd::jsg {

// Reserved tag values for jsg-internal uses (non-templated).
enum class JsgExternalIds : uint16_t {
  kCapnpSchema = 1,
  kCapnpInterfaceMethod = 2,
};

}  // namespace workerd::jsg
