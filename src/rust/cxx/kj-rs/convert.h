#pragma once

// kj-rs/convert.h - Rust/C++ Interoperability Utilities for KJ
//
// This header provides seamless integration between Rust types and KJ (Cap'n Proto's C++ library)
// types, enabling efficient and safe data exchange across the language boundary.
//
// ============================================================================
// USAGE OVERVIEW
// ============================================================================
//
// Converting C++ kj arrays, strings, etc to Rust:
// - kjObject.as<Rust>() - creates zero-copy read-only Rust view
// - kjObject.as<RustMutable>() - creates zero-copy mutable Rust view
// - kjObject.as<RustCopy>() - creates owned Rust copy (safe byte arrays)
// - kjObject.as<RustUncheckedUtf8>() - creates Rust string
// - kjObject.as<RustCopyUncheckedUtf8>() - creates owned Rust string (assumes valid UTF-8)
//
// Converting Rust to C++ kj objects:
// - from<Rust>(rustObject) - creates zero-copy C++ view
// - from<RustCopy>(rustObject) - creates owned C++ copy
// - kj::str(rustString/rustSlice/rustVec) - automatic conversion (via KJ_STRINGIFY)
// - kj::hashCode(rustString/rustSlice/rustVec) - automatic hash computation (via KJ_HASHCODE)
//
// ============================================================================
// ARRAY/COLLECTION CONVERSIONS
// ============================================================================
//
// Zero-copy conversions from Rust to C++:
// - from<Rust>(rust::Vec<T>) -> kj::ArrayPtr<const T>
// - from<Rust>(rust::Slice<T>) -> kj::ArrayPtr<T>
//
// Zero-copy conversions from C++ to Rust (read-only):
// - kjArray.as<Rust>() -> rust::Slice<const T>
//
// Zero-copy conversions from C++ to Rust (mutable):
// - kjArray.as<RustMutable>() -> rust::Slice<T>
// - kjArrayPtr.as<RustMutable>() -> rust::Slice<T>
//
// Owned conversions from C++ to Rust (copying):
// - kjArrayPtr.as<RustCopy>() -> rust::Vec<T>
//
// ============================================================================
// STRING CONVERSIONS
// ============================================================================
//
// IMPORTANT: Rust strings require valid UTF-8, but KJ strings don't!
// This library provides both SAFE and UNSAFE string conversion options.
//
// --- RUST TO C++ STRING CONVERSIONS ---
//
// Zero-copy (always safe):
// - from<Rust>(rust::String) -> kj::ArrayPtr<const char>
// - from<Rust>(rust::str) -> kj::ArrayPtr<const char>
//
// Owned copies (always safe):
// - from<RustCopy>(rust::Slice<rust::str>) -> kj::Array<kj::String>
// - from<RustCopy>(rust::Vec<rust::String>) -> kj::Array<kj::String>
// - kj::str(rust::str) -> kj::String
// - kj::str(rust::String) -> kj::String
//
// --- C++ TO RUST STRING CONVERSIONS (SAFE) ---
//
// Returns raw bytes - use std::str::from_utf8() or from_utf8_lossy() on Rust side:
// - kjString.as<Rust>() -> rust::Slice<const char>  // Safe for non-UTF-8 data
// - kjStringPtr.as<Rust>() -> rust::Slice<const char>  // Safe for non-UTF-8 data
// - kjConstString.as<Rust>() -> rust::Slice<const char>  // Safe for non-UTF-8 data
//
// Returns owned bytes - use std::str::from_utf8() or from_utf8_lossy() on Rust side:
// - kjStringPtr.as<RustCopy>() -> rust::Vec<char>  // Safe byte array
// - kjConstString.as<RustCopy>() -> rust::Vec<char>  // Safe byte array
//
// --- C++ TO RUST STRING CONVERSIONS (UNSAFE) ---
//
// ⚠️  WARNING: These assume valid UTF-8 and rust code might panic or cause undefined behavior
//              if the KJ string contains invalid UTF-8 bytes!
//
// Zero-copy (UNSAFE - assumes valid UTF-8):
// - kjString.as<RustUncheckedUtf8>() -> rust::String
// - kjStringPtr.as<RustUncheckedUtf8>() -> rust::Str
// - kjConstString.as<RustUncheckedUtf8>() -> rust::Str
//
// Owned copies (UNSAFE - assumes valid UTF-8):
// - kjStringPtr.as<RustCopyUncheckedUtf8>() -> rust::String
// - kjConstString.as<RustCopyUncheckedUtf8>() -> rust::String
//
// --- AUTOMATIC STRING CONVERSIONS ---
//
// These work with kj::str() and kj::hashCode() automatically:
// - kj::str(rust::String) - uses KJ_STRINGIFY for seamless string conversion
// - kj::str(rust::Slice<const char>) - uses KJ_STRINGIFY for slice conversion
// - kj::str(rust::Vec<char>) - uses KJ_STRINGIFY for vector conversion
// - kj::hashCode(rust::String) - uses KJ_HASHCODE for hash computation
// - kj::hashCode(rust::Slice<const char>) - uses KJ_HASHCODE for slice hashing
// - kj::hashCode(rust::Vec<char>) - uses KJ_HASHCODE for vector hashing
//
// ============================================================================
// EXAMPLES
// ============================================================================
//
// Basic usage patterns:
//
// // Convert Rust to C++:
// kj::ArrayPtr<const int> cppView = kj::from<Rust>(rustVec);
//
// // Convert C++ to Rust (read-only, safe):
// rust::Slice<const char> rustBytes = kjString.as<Rust>();
// // Then in Rust: std::str::from_utf8(&rustBytes) or from_utf8_lossy(&rustBytes)
//
// // Convert C++ to Rust (mutable):
// rust::Slice<int> rustMutableView = cppArray.as<RustMutable>();
//
// // Convert C++ to Rust (copying, safe):
// rust::Vec<char> rustOwnedBytes = kjStr.as<RustCopy>();
//
// // Convert C++ to Rust (unsafe, assumes valid UTF-8):
// rust::String rustStr = kjStr.as<RustUncheckedUtf8>();  // UNSAFE!
//
// // Automatic string conversion:
// kj::String cppStr = kj::str(rustStr);  // via KJ_STRINGIFY
// kj::String cppStr2 = kj::str(rustSlice);  // also works with slices/vecs
//

#include <rust/cxx.h>

#include <kj/common.h>
#include <kj/hash.h>
#include <kj/string.h>

namespace rust {

// Automatic KJ Integration Functions - enable ADL for kj::str() and kj::hashCode()

/// Converts rust::String to kj::ArrayPtr - called by kj::str(rustString)
inline auto KJ_STRINGIFY(const ::rust::String& str) {
  // HACK: rust::String is not null-terminated, so we use kj::ArrayPtr instead
  // which usually acts like kj::StringPtr but does not rely on null
  // termination.
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

/// Converts rust::str to kj::ArrayPtr - called by kj::str(rustStr)
inline auto KJ_STRINGIFY(const ::rust::str& str) {
  // HACK: rust::str is not null-terminated, so we use kj::ArrayPtr instead
  // which usually acts like kj::StringPtr but does not rely on null
  // termination.
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

/// Converts rust::Slice<const char> to kj::ArrayPtr - called by kj::str(rustSlice)
inline auto KJ_STRINGIFY(const ::rust::Slice<const char>& str) {
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

/// Converts rust::Vec<const char> to kj::ArrayPtr - called by kj::str(rustVec)
inline auto KJ_STRINGIFY(const ::rust::Vec<char>& str) {
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

/// Hash code for rust::String - called by kj::hashCode(rustString)
inline auto KJ_HASHCODE(const ::rust::String& str) {
  return kj::hashCode(kj::toCharSequence(str));
}

/// Hash code for rust::str - called by kj::hashCode(rustStr)
inline auto KJ_HASHCODE(const ::rust::str& str) {
  return kj::hashCode(kj::toCharSequence(str));
}

/// Hash code for rust::Slice<const char> - called by kj::hashCode(rustSlice)
inline auto KJ_HASHCODE(const ::rust::Slice<const char>& str) {
  return kj::hashCode(kj::toCharSequence(str));
}

/// Hash code for rust::Vec<const char> - called by kj::hashCode(rustVec)
inline auto KJ_HASHCODE(const ::rust::Vec<char>& str) {
  return kj::hashCode(kj::toCharSequence(str));
}

}  // namespace rust

namespace kj_rs {

// KJ to Rust conversion utilities with different ownership semantics

/// Zero-copy read-only Rust views: kjObject.as<Rust>() and kj::from<Rust>(kjObject)
struct Rust {};

/// kjArrayPtr.as<Rust>()
template <typename T>
inline ::rust::Slice<const T> asImpl(Rust*, const kj::ArrayPtr<T>& arr) {
  return ::rust::Slice<const T>(arr.begin(), arr.size());
}

/// kjArray.as<Rust>()
template <typename T>
inline ::rust::Slice<const T> asImpl(Rust*, const kj::Array<T>& arr) {
  return ::rust::Slice<const T>(arr.begin(), arr.size());
}

/// kjString.as<Rust>()
inline ::rust::Slice<const char> asImpl(Rust*, const kj::String& str) {
  return ::rust::Slice(str.begin(), str.size());
}

/// kjStringPtr.as<Rust>()
inline ::rust::Slice<const char> asImpl(Rust*, const kj::StringPtr& str) {
  return ::rust::Slice(str.begin(), str.size());
}

/// kjConstString.as<Rust>()
inline ::rust::Slice<const char> asImpl(Rust*, const kj::ConstString& str) {
  return ::rust::Slice(str.begin(), str.size());
}

/// from<Rust>(rustVec) - Zero-copy read-only view
template <typename T>
inline kj::ArrayPtr<const T> fromImpl(Rust*, const ::rust::Vec<T>& vec) {
  return kj::ArrayPtr<const T>(vec.data(), vec.size());
}

/// from<Rust>(rustSlice) - Zero-copy slice view
template <typename T>
inline kj::ArrayPtr<T> fromImpl(Rust*, const ::rust::Slice<T>& slice) {
  return kj::ArrayPtr<T>(slice.data(), slice.size());
}

/// from<Rust>(rustString) - Zero-copy string chars (not null-terminated)
inline kj::ArrayPtr<const char> fromImpl(Rust*, const ::rust::String& str) {
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

/// from<Rust>(rustStr) - Zero-copy string slice chars (not null-terminated)
inline kj::ArrayPtr<const char> fromImpl(Rust*, const ::rust::Str& str) {
  return kj::ArrayPtr<const char>(str.data(), str.size());
}

/// Owned Rust copies: kjObject.as<RustCopy>() and from<RustCopy>(kjObject)
struct RustCopy {};

/// kjArrayPtr.as<RustCopy>()
///
/// Taken by value so it binds equally to const and mutable `kj::ArrayPtr`s, to temporaries (e.g.
/// `arr.asBytes().as<RustCopy>()`), and to lvalues -- a `kj::ArrayPtr` is just a pointer+size, so
/// the copy is free. `kj::Decay` drops the element's constness so a `kj::ArrayPtr<const T>` still
/// yields a `rust::Vec<T>`.
template <typename T>
inline ::rust::Vec<kj::Decay<T>> asImpl(RustCopy*, kj::ArrayPtr<T> arr) {
  ::rust::Vec<kj::Decay<T>> result;
  result.reserve(arr.size());
  for (auto& t: arr) {
    result.push_back(t);
  }
  return result;
}

/// kjArray.as<RustCopy>()
template <typename T>
inline ::rust::Vec<kj::Decay<T>> asImpl(RustCopy*, const kj::Array<T>& arr) {
  return asImpl(static_cast<RustCopy*>(nullptr), arr.asPtr());
}

/// kjStringPtr.as<RustCopy>()
inline ::rust::Vec<char> asImpl(RustCopy*, const kj::StringPtr& str) {
  auto ptr = str.asArray();
  return asImpl(static_cast<RustCopy*>(nullptr), ptr);
}

/// kjConstString.as<RustCopy>()
inline ::rust::Vec<char> asImpl(RustCopy*, const kj::ConstString& str) {
  auto ptr = str.asArray();
  return asImpl(static_cast<RustCopy*>(nullptr), ptr);
}

/// from<RustCopy>(rustSliceOfStrs) - Copy slice of strs to null-terminated KJ strings
inline kj::Array<kj::String> fromImpl(RustCopy*, ::rust::Slice<::rust::str> slice) {
  auto res = kj::heapArrayBuilder<kj::String>(slice.size());
  for (auto& entry: slice) {
    res.add(kj::str(entry));
  }
  return res.finish();
}

/// from<RustCopy>(rustVecOfStrings) - Copy string vector to null-terminated KJ strings
inline kj::Array<kj::String> fromImpl(RustCopy*, const ::rust::Vec<::rust::String>& vec) {
  auto res = kj::heapArrayBuilder<kj::String>(vec.size());
  for (auto& entry: vec) {
    res.add(kj::str(entry));
  }
  return res.finish();
}

/// Mutable Rust views
struct RustMutable {};

/// kjArrayPtr.as<RustMutable>()
template <typename T>
inline ::rust::Slice<T> asImpl(RustMutable*, kj::ArrayPtr<T>& arr) {
  return ::rust::Slice<T>(arr.begin(), arr.size());
}

/// kjArray.as<RustMutable>()
template <typename T>
inline ::rust::Slice<T> asImpl(RustMutable*, kj::Array<T>& arr) {
  return ::rust::Slice<T>(arr.begin(), arr.size());
}

// Rust strings require valid utf8 content, which is not enforced by `kj::String`.
// Passing invalid utf8 to `rust::String` could result in panics and other unexpected behaviour.
// Use this struct to convert `kj::String` to `rust::String` without checking for valid utf8
// when you are confident about the content of the string or do not care about the consequences.
// It is also safer to convert strings to slices and use `from_utf8_lossy` or friends on rust side.
struct RustUncheckedUtf8 {};

/// kjString.as<RustUncheckedUtf8>()
inline ::rust::String asImpl(RustUncheckedUtf8*, const kj::String& str) {
  return ::rust::String(str.begin(), str.size());
}

/// kjStringPtr.as<RustUncheckedUtf8>()
inline ::rust::Str asImpl(RustUncheckedUtf8*, const kj::StringPtr& str) {
  return ::rust::Str(str.begin(), str.size());
}

/// kjConstString.as<RustUncheckedUtf8>()
inline ::rust::Str asImpl(RustUncheckedUtf8*, const kj::ConstString& str) {
  return ::rust::Str(str.begin(), str.size());
}

// Copying conversion for string types. See comment for `RustUncheckedUtf8` for details.
struct RustCopyUncheckedUtf8 {};

/// kjStringPtr.as<RustCopyUncheckedUtf8>()
inline ::rust::String asImpl(RustCopyUncheckedUtf8*, const kj::StringPtr& str) {
  return ::rust::String(str.begin(), str.size());
}

/// kjConstString.as<RustCopyUncheckedUtf8>()
inline ::rust::String asImpl(RustCopyUncheckedUtf8*, const kj::ConstString& str) {
  return ::rust::String(str.begin(), str.size());
}

}  // namespace kj_rs
