#pragma once

#include <v8-version.h>

#include <cstdint>
#include <type_traits>

namespace workerd::jsg {

// Reserved tag values for jsg-internal uses (non-templated).
enum class JsgExternalIds : uint16_t {
  kCapnpSchema = 1,
  kCapnpInterfaceMethod = 2,
};

}  // namespace workerd::jsg
