#pragma once

#include <v8-version.h>

#include <cstdint>
#include <type_traits>

// V8 14.3+ supports external pointer type tags for security.
#define V8_HAS_EXTERNAL_POINTER_TAGS                                                               \
  (V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 3))

// External pointer type tags for v8::External objects.
// These tags are used to prevent type confusion if memory in the V8 Heap
// sandbox gets corrupted.
//
// Usage:
// Types used with Lock::external<T>() and JsValue::tryGetExternal<T>() must
// define a static constexpr kExternalId field. The field can be an enum value
// or a small integer, and will be cast to uint16_t.
//
// Example:
//   // Define an enum for your external pointer tags
//   enum class MyExternalIds : uint16_t {
//     kFoo = 1,
//     kBar = 2,
//   };
//
//   class MyType {
//   public:
//     static constexpr MyExternalIds kExternalId = MyExternalIds::kFoo;
//     // ... your class definition
//   };
//
//   // Storing a pointer using Lock::external<MyType>()
//   auto ext = js.external(&myInstance);
//
//   // Retrieving the pointer using JsValue::tryGetExternal<MyType>()
//   KJ_IF_SOME(ref, JsValue::tryGetExternal<MyType>(js, value)) {
//     // use ref
//   }

namespace workerd::jsg {

// Helper to get the external tag for a type T.
// T must have a static constexpr kExternalId member.
template <typename T>
struct ExternalTagFor {
  static constexpr uint16_t get() {
    return static_cast<uint16_t>(T::kExternalId);
  }
};

// Reserved tag values for jsg-internal uses (non-templated).
enum class JsgExternalIds : uint16_t {
  kCapnpSchema = 1,
  kCapnpInterfaceMethod = 2,
};

}  // namespace workerd::jsg
