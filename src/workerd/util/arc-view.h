#pragma once

#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd {

// Helpers for `kj::Arc` over read-only view types (`kj::StringPtr`, `kj::ArrayPtr<const T>`).
// Such an Arc stores the view inline and shares the refcount of whatever owns the bytes, so a
// view of a capnp message field, a heap string, or a transient buffer all have the same type and
// the same lifetime rule: the bytes live as long as any Arc pointing at them.

// Moves an owned string onto the heap and returns a shared view of it.
inline kj::Arc<kj::StringPtr> arcView(kj::String value) {
  return kj::arc<kj::String>(kj::mv(value)).project([](const kj::String& s) { return s.asPtr(); });
}

// Moves an owned string onto the heap and returns a shared character-array view of it.
inline kj::Arc<kj::ArrayPtr<const char>> arcCharView(kj::String value) {
  return kj::arc<kj::String>(kj::mv(value))
      .project([](const kj::String& s) -> kj::ArrayPtr<const char> { return s.asPtr(); });
}

// Moves an owned array onto the heap and returns a shared read-only view of it.
template <typename T>
kj::Arc<kj::ArrayPtr<const T>> arcView(kj::Array<T> value) {
  return kj::arc<kj::Array<T>>(kj::mv(value))
      .project([](const kj::Array<T>& a) -> kj::ArrayPtr<const T> { return a.asPtr(); });
}

// Convert a shared string view to a character-array view, sharing the same ownership claim.
inline kj::Arc<kj::ArrayPtr<const char>> asCharView(kj::Arc<kj::StringPtr> text) {
  return kj::mv(text).project([](const kj::StringPtr& s) -> kj::ArrayPtr<const char> { return s; });
}

// Reinterpret a shared text view as bytes, sharing the same ownership claim.
inline kj::Arc<kj::ArrayPtr<const kj::byte>> asByteView(kj::Arc<kj::StringPtr> text) {
  return kj::mv(text).project([](const kj::StringPtr& s) { return s.asBytes(); });
}
inline kj::Arc<kj::ArrayPtr<const kj::byte>> asByteView(kj::Arc<kj::ArrayPtr<const char>> chars) {
  return kj::mv(chars).project([](const kj::ArrayPtr<const char>& c) { return c.asBytes(); });
}

}  // namespace workerd
