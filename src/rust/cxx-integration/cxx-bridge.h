#pragma once

#include <rust/cxx.h>

#include <kj/hash.h>
#include <kj/string.h>

namespace rust {

inline auto KJ_STRINGIFY(const ::rust::String& str) {
  // HACK: rust::String is not null-terminated, so we use kj::ArrayPtr instead
  // which usually acts like kj::StringPtr but does not rely on null
  // termination.
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

inline auto KJ_STRINGIFY(const ::rust::str& str) {
  // HACK: rust::str is not null-terminated, so we use kj::ArrayPtr instead
  // which usually acts like kj::StringPtr but does not rely on null
  // termination.
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

inline auto KJ_HASHCODE(const ::rust::String& str) { return kj::hashCode(kj::toCharSequence(str)); }

inline auto KJ_HASHCODE(const ::rust::str& str) { return kj::hashCode(kj::toCharSequence(str)); }

}  // namespace rust

namespace workerd {

// conversions to kj::ArrayPtr

template <typename T>
inline kj::ArrayPtr<const T> fromRust(const ::rust::Vec<T>& vec) {
  return kj::ArrayPtr<const T>(vec.data(), vec.size());
}

template <typename T>
inline kj::ArrayPtr<T> fromRust(const ::rust::Slice<T>& slice) {
  return kj::ArrayPtr<T>(slice.data(), slice.size());
}

inline kj::ArrayPtr<const char> fromRust(const ::rust::String& str) {
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

inline kj::ArrayPtr<const char> fromRust(const ::rust::Str& str) {
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

inline kj::Array<kj::String> fromRust(::rust::Vec<::rust::String> vec) {
  auto res = kj::heapArrayBuilder<kj::String>(vec.size());
  for (auto& entry: vec) {
    res.add(kj::str(entry.c_str()));
  }
  return res.finish();
}

struct Rust {
  template <typename T>
  static ::rust::Slice<const T> from(const kj::ArrayPtr<T>* arr) {
    return ::rust::Slice<const T>(arr->begin(), arr->size());
  }

  template <typename T>
  static ::rust::Slice<const T> from(const kj::Array<T>* arr) {
    return ::rust::Slice<const T>(arr->begin(), arr->size());
  }

  static ::rust::String from(const kj::String* str) {
    return ::rust::String(str->begin(), str->size());
  }

  static ::rust::Str from(const kj::StringPtr* str) {
    return ::rust::Str(str->begin(), str->size());
  }
};

// Create rust objects by making a copy of data.
struct RustCopy {
  static ::rust::String from(const kj::StringPtr* str) {
    return ::rust::String(str->begin(), str->size());
  }

  template <typename T>
  static ::rust::Vec<T> from(kj::ArrayPtr<const T>* arr) {
    ::rust::Vec<T> result;
    result.reserve(arr->size());
    for (auto& t : *arr) {
      result.push_back(t);
    }
    return result;
  }
};

struct RustMutable {
  template <typename T>
  static ::rust::Slice<T> from(kj::ArrayPtr<T>* arr) {
    return ::rust::Slice<T>(arr->begin(), arr->size());
  }

  template <typename T>
  static ::rust::Slice<T> from(kj::Array<T>* arr) {
    return ::rust::Slice<T>(arr->begin(), arr->size());
  }
};

}  // namespace edgeworker
