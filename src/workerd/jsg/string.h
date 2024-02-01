// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jsg.h"
#include <compare>

namespace workerd::jsg {

class UsvStringPtr;
class UsvString;
class UsvStringBuilder;

// In most standard Web Platform APIs, strings are generally handled as either
// "ByteString", "USVString", or "DOMString", with "USVString" being the most
// common for APIs like URL, URLPattern, the Encoding spec, etc.
//
// Per the Web IDL spec: "The USVString type corresponds to the set of all possible"
// sequences of Unicode scalar values, which are all of the Unicode code points apart
// from the surrogate code points."
//
// Also per the Web UDL spec, the process of converting a JavaScript value into
// a USVString is:
//
//   Let string be the result of converting V to a DOMString.
//
//   Return an IDL USVString value that is the result of converting string to a sequence
//   of Unicode scalar values.
//
//   An IDL USVString value is converted to an ECMAScript value by running the following algorithm:
//
//     Let scalarValues be the sequence of Unicode scalar values the USVString represents.
//
//     Let string be the sequence of code units that results from encoding scalarValues in UTF-16.
//
//     Return the String value that represents the same sequence of code units as string.
//
// In other words, take the v8::Local<v8::Value> and convert it, if possible, into a
// v8::Local<v8::String>. Write that v8::String to a two-byte (utf-16) representation,
// replacing any unpaired surrogates with the standard Unicode replacement char \uFFFD.
// The USVString is then represented by the sequence of Unicode Codepoints contained in
// the resulting array of 16-bit code units, taking properly paired surrogates into
// account.
//
//
// Usage:
//
// To create a new jsg::UsvString, use either one of the jsg::usv() function variants:
//
//   auto usvstring1 = jsg::usv("hello");                // copy from string literals.
//   auto usvstring2 = jsg::usv(kj::str("hello"));       // copy from kj::Strings
//   auto usvstring3 = jsg::usv(kj::StringPtr("hello"))  // copy from kj::StringPtrs
//   auto usvstring3 = jsg::usv(v8Value, isolate);       // copy from v8::Local<*> values
//   auto usvstring4 = jsg::usv(otherUsvString);         // copy from another UsvString
//   auto usvstring5 = jsg::usv(otherUsvString.asPtr()); // copy from a UsvStringPtr
//
// Or use a jsg::UsvStringBuilder:
//
//   jsg::UsvStringBuilder builder;
//   builder.add(0x1f607);  // Append an individual Unicode codepoint.
//   builder.add('h', 'e', 'l', 'l', 'o');
//   builder.addAll(jsg::usv("world"));
//   auto usvstring6 = builder.finish();
//
// The jsg::UsvStringBuilder allows constructing a UsvString one Unicode codepoint at
// a time or from other UsvStrings, kj::Strings, string literals, and so on.
//
// It is important to know that every UsvString has a heap-allocated internal storage
// in the form of a kj::Array<uint32_t>. When a string literal or kj::String is used
// to create a UsvString, a UTF-8 encoding is assumed and the content will be transcoded
// into a UTF-32 representation. In performance sensitive parts of the code, these
// additional heap allocations can be expensive. If you find yourself doing multiple
// conversions of the same string literals or kj::String values (such as performing
// multiple comparison operations against the same value), then it is advisable just to
// create UsvString values once that can be reused.
//
// In other words, this is bad:
//
//   auto str = jsg::usv("hello");
//   for (int n = 0; n < 100; n++) {
//     // Don't do this.
//     if (str == jsg::usv("there")) { /** ... **/ }
//   }
//
// Do this instead:
//
//   auto str = jsg::usv("hello");
//   auto other = jsg::usv("there");
//   for (int n = 0; n < 100; n++) {
//     if (str == other) { /** ... **/ }
//   }
//
// Converting to kj::Strings:
//
// The jsg::UsvString supports the KJ_STRINGIFY convention, which allows easily
// creating a kj::String using the kj::str() function. For example:
//
//   auto kjStr = kj::str(jsg::usv("hello"));
//
// The kj::String created will be a UTF-8 encoded copy of the UsvString's contents.
// Again, when performance is a consideration, try to do these conversions sparingly.
//
// Converting to a V8 String:
//
// To create a v8::String from a UsvString, use the jsg::v8Str() method:
//
//   v8::Local<v8::String> string = jsg::v8Str(isolate, jsg::usv("hello"));
//
// Iterating over Unicode Codepoints:
//
// The jsg::UsvStringIterator is used to iterate over the set of Unicode codepoints
// contained in a jsg::UsvString (or jsg::UsvStringPtr). These follow the standard
// c++ iterator pattern:
//
//   auto str = jsg::usv("hello");
//   for (auto it = str.begin(); it < str.end(); ++it) {}
//
// The iterator supports pre- and post- increment operators (e.g. ++it and it++ )
// and implements a bool operator that will evaluate to false when the iterator
// has reached the end:
//
//   auto it = str.begin();
//   while (it) { ++it; }
//
// Access the current codepoint using the * operator:
//
//   auto it = str.begin();
//   auto firstCodepoint = *it;
//
// All Unicode codepoints are uint32_t values.
//
// Both the jsg::UsvString and jsg::UsvStringPtr objects expose a getCodepointAt(n)
// method that can retrieve the n-th Unicode codepoint in the sequence.
//
// Slicing UsvString's and jsg::UsvStringPtr
//
// A jsg::UsvStringPtr shares the memory storage of a parent jsg::UsvString. It is
// very similar to kj::StringPtr and kj::ArrayPtr and is used in much the same way.
//
// All jsg::UsvString's have a codepoint-aware slice() operation that returns a
// jsg::UsvStringPtr to the identified range:
//
//   auto str = usv::str("hëllo");
//   auto ptr = str.slice(2, 4);
//   KJ_DBG(ptr);  // "ll"
//
// The index values in the slice() operation identify codepoint offsets.
//
// Keep in mind that the lifetime of the jsg::UsvStringPtr is bound to it's parent
// jsg::UsvString.


// Iterates over the 32-bit unicode codepoints in a UsvString or UsvStringPtr
class UsvStringIterator {
public:
  uint32_t operator* () const;

  UsvStringIterator& operator++();
  UsvStringIterator operator++(int);
  UsvStringIterator& operator+=(int);
  UsvStringIterator operator+(int);

  UsvStringIterator& operator--();
  UsvStringIterator operator--(int);
  UsvStringIterator& operator-=(int);
  UsvStringIterator operator-(int);

  inline explicit operator bool() const { return pos < ptr.size(); }

  inline bool operator<(UsvStringIterator& other) const {
    return pos < other.pos;
  }

  inline bool operator<=(UsvStringIterator& other) const {
    return pos <= other.pos;
  }

  inline bool operator>(UsvStringIterator& other) const {
    return pos > other.pos;
  }

  inline bool operator>=(UsvStringIterator& other) const {
    return pos >= other.pos;
  }

  inline bool operator==(const UsvStringIterator& other) const {
    return pos == other.pos;
  }

  // Informational. Identifies the iterators current codepoint position.
  // When position() == size(), this iterator has reached the end.
  inline size_t position() const { return pos; }

  // Informational. Identifies the maximum number of codepoints.
  inline size_t size() const { return ptr.size(); }

private:
  explicit inline UsvStringIterator(kj::ArrayPtr<uint32_t> ptr, size_t pos) : ptr(ptr), pos(pos) {}

  kj::ArrayPtr<uint32_t> ptr;
  size_t pos = 0;

  friend class UsvStringPtr;
  friend class UsvString;
};

// A humble pointer to a UsvString.
// Shares the same underlying storage as the UsvString.
class UsvStringPtr: public kj::DisallowConstCopy {
public:
  UsvStringPtr(UsvStringPtr&&) = default;
  UsvStringPtr& operator=(UsvStringPtr&&) = default;
  UsvStringPtr(UsvStringPtr&) = default;
  UsvStringPtr& operator=(UsvStringPtr&) = default;

  // Return a copy of this UsvStringPtr as a UTF-8 encoded kj::String.
  kj::String toStr() KJ_WARN_UNUSED_RESULT;

  const kj::String toStr() const KJ_WARN_UNUSED_RESULT;

  // Return a copy of this UsvStringPtr as an array of UTF-16 code units.
  kj::Array<uint16_t> toUtf16() KJ_WARN_UNUSED_RESULT;

  // Return a copy of this UsvStringPtr as an array of UTF-16 code units.
  const kj::Array<const uint16_t> toUtf16() const KJ_WARN_UNUSED_RESULT;

  // Return a copy of this UsvStringPtr.
  UsvString clone() KJ_WARN_UNUSED_RESULT;

  uint32_t getCodepointAt(size_t index) const;
  uint32_t operator[](size_t index) const { return getCodepointAt(index); }

  inline bool operator==(UsvStringPtr& other) { return ptr == other.ptr; }

  inline bool operator==(const UsvStringPtr& other) const { return ptr == other.ptr; }

  std::weak_ordering operator<=>(const UsvString& other) const;
  std::weak_ordering operator<=>(const UsvStringPtr& other) const;
  std::weak_ordering operator<=>(UsvString& other);
  std::weak_ordering operator<=>(UsvStringPtr& other);

  // Returns the first Unicode codepoint in the sequence.
  inline uint32_t first() const { return getCodepointAt(0); }

  // Returns the last Unicode codepoint in the sequence.
  inline uint32_t last() const { return getCodepointAt(size() - 1); }

  kj::Maybe<size_t> lastIndexOf(uint32_t codepoint);

  inline UsvStringIterator begin() KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT {
    return UsvStringIterator(ptr, 0);
  }
  inline UsvStringIterator end() KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT {
    return UsvStringIterator(ptr, size());
  }

  // Returns the counted number of unicode codepoints in the string.
  inline size_t size() const { return ptr.size(); }

  inline bool empty() const { return size() == 0; }

  // Informational. Returns a pointer to the underlying storage.
  inline kj::ArrayPtr<uint32_t> storage() KJ_LIFETIMEBOUND { return ptr; }

  inline const kj::ArrayPtr<const uint32_t> storage() const KJ_LIFETIMEBOUND { return ptr; }

  UsvStringPtr slice(size_t start, size_t end) KJ_LIFETIMEBOUND;
  inline UsvStringPtr slice(size_t start) KJ_LIFETIMEBOUND { return slice(start, size()); }
  inline UsvStringPtr slice(UsvStringIterator start) KJ_LIFETIMEBOUND {
    return slice(start.position());
  };
  inline UsvStringPtr slice(UsvStringIterator start, UsvStringIterator end) KJ_LIFETIMEBOUND {
    return slice(start.position(), end.position());
  }

private:
  UsvStringPtr(kj::ArrayPtr<uint32_t> ptr) : ptr(ptr) {}

  kj::ArrayPtr<uint32_t> ptr;

  friend class UsvString;
  friend class UsvStringBuilder;
};


// A sequence of Unicode Codepoints (a.k.a Unicode scalar values).
// Unpaired surrogate codepoints are automatically converted into
// the standard 0xFFFD replacement character on creation.
//
// Internally, a UsvString is an array of 2-byte code units.
// Every Unicode codepoint is represented by either one or two
// code units each.
class UsvString {
public:
  UsvString() = default;

  // Takes over ownership of the array of unicode code units. Specifically,
  // does not copy or heap allocate anything. The number of Unicode codepoints
  // will be calculated.
  UsvString(kj::Array<uint32_t> buffer) : buffer(kj::mv(buffer)) {}

  UsvString(UsvString&& other) = default;
  UsvString& operator=(UsvString&& other) = default;

  KJ_DISALLOW_COPY(UsvString);

  // Return a copy of this UsvString.
  UsvString clone() KJ_WARN_UNUSED_RESULT;

  // Return a copy of this UsvString as a UTF-8 encoded kj::String.
  kj::String toStr() KJ_WARN_UNUSED_RESULT;

  const kj::String toStr() const KJ_WARN_UNUSED_RESULT;

  // Return a copy of this UsvString as an array of UTF-16 code units.
  kj::Array<uint16_t> toUtf16() KJ_WARN_UNUSED_RESULT;

  // Return a copy of this UsvString as an array of UTF-16 code units.
  const kj::Array<const uint16_t> toUtf16() const KJ_WARN_UNUSED_RESULT;

  inline operator UsvStringPtr() KJ_LIFETIMEBOUND { return UsvStringPtr(buffer); }
  inline UsvStringPtr asPtr() KJ_LIFETIMEBOUND { return UsvStringPtr(*this); }

  uint32_t getCodepointAt(size_t index) const;
  uint32_t operator[](size_t index) const { return getCodepointAt(index); }

  inline bool operator==(UsvString& other) { return buffer == other.buffer; }

  inline bool operator==(const UsvString& other) const { return buffer == other.buffer; }

  std::weak_ordering operator<=>(const UsvString& other) const;
  std::weak_ordering operator<=>(const UsvStringPtr& other) const;
  std::weak_ordering operator<=>(UsvString& other);
  std::weak_ordering operator<=>(UsvStringPtr& other);

  // Returns the first Unicode codepoint in the sequence.
  inline uint32_t first() const { return getCodepointAt(0); }

  // Returns the last Unicode codepoint in the sequence.
  inline uint32_t last() const { return getCodepointAt(size() - 1); }

  kj::Maybe<size_t> lastIndexOf(uint32_t codepoint);

  inline UsvStringIterator begin() KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT {
    return UsvStringIterator(buffer, 0);
  }
  inline UsvStringIterator end() KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT {
    return UsvStringIterator(buffer, size());
  }

  // Returns the counted number of unicode codepoints in the string.
  inline size_t size() const { return buffer.size(); }

  inline bool empty() const { return size() == 0; }

  // Informational. Returns a pointer to the underlying storage.
  inline kj::ArrayPtr<uint32_t> storage() KJ_LIFETIMEBOUND { return buffer; }

  inline const kj::ArrayPtr<const uint32_t> storage() const KJ_LIFETIMEBOUND { return buffer; }

  UsvStringPtr slice(size_t start, size_t end) KJ_LIFETIMEBOUND;
  inline UsvStringPtr slice(size_t start) KJ_LIFETIMEBOUND { return slice(start, size()); }
  inline UsvStringPtr slice(UsvStringIterator start) KJ_LIFETIMEBOUND {
    return slice(start.position());
  };
  inline UsvStringPtr slice(UsvStringIterator start, UsvStringIterator end) KJ_LIFETIMEBOUND {
    return slice(start.position(), end.position());
  }

  JSG_MEMORY_INFO(UsvString) {
    tracker.trackField("buffer", buffer);
  }

private:
  kj::Array<uint32_t> buffer;

  friend class UsvStringBuilder;
  friend class UsvStringPtr;
};

inline KJ_WARN_UNUSED_RESULT UsvString usv() { return UsvString(); }

// Make a UsvString from a kj::StringPtr (assumed to by UTF-8 encoded)
// reinterpreted as a sequence of UTF-16 Unicode code units. The underlying
// storage of utf16_t code units will be heap allocated.
KJ_WARN_UNUSED_RESULT UsvString usv(kj::ArrayPtr<const char> string);

inline KJ_WARN_UNUSED_RESULT UsvString usv(UsvString&& other) { return kj::mv(other); }
inline KJ_WARN_UNUSED_RESULT UsvString usv(UsvStringPtr other) { return other.clone(); }

// Make a UsvString from a string literal (assumed to be UTF-8 encoded)
// reinterpreted as a sequence of UTF-16 Unicode code units. The underlying
// storage of utf16_t code units will be heap allocated.
inline KJ_WARN_UNUSED_RESULT UsvString usv(const char* string) {
  return usv(kj::toCharSequence(string));
}

KJ_WARN_UNUSED_RESULT UsvString usv(kj::ArrayPtr<uint16_t> string);

// Make a UsvString from a JavaScript value reinterpreted first as a string,
// and then as a sequence of utf16_t Unicode code units. The underlying
// storage of utf16_t code units will be heap allocated.
KJ_WARN_UNUSED_RESULT
UsvString usv(v8::Isolate* isolate, v8::Local<v8::Value> value);

// Make a JavaScript String in v8's Heap from a UsvString.
KJ_WARN_UNUSED_RESULT
UsvString usv(Lock& js, const JsValue& value);
// Make a UsvString from a JavaScript value reinterpreted first as a string,
// and then as a sequence of utf16_t Unicode code units. The underlying
// storage of utf16_t code units will be heap allocated.

KJ_WARN_UNUSED_RESULT
v8::Local<v8::String> v8Str(v8::Isolate* isolate,
                            UsvStringPtr str,
                            v8::NewStringType newType = v8::NewStringType::kNormal);

// Allows incrementally constructing a UsvString.
class UsvStringBuilder {
public:
  UsvStringBuilder() = default;
  UsvStringBuilder(size_t reservedSize) { reserve(reservedSize); }
  UsvStringBuilder(UsvStringBuilder&& other) = default;
  UsvStringBuilder& operator=(UsvStringBuilder&& other) = default;

  KJ_DISALLOW_COPY(UsvStringBuilder);

  inline operator UsvStringPtr() KJ_LIFETIMEBOUND { return UsvStringPtr(buffer); }
  inline UsvStringPtr asPtr() KJ_LIFETIMEBOUND { return UsvStringPtr(*this); }

  void add(uint32_t codepoint);

  inline void add(uint32_t codepoint, auto&&... codepoints) {
    add(codepoint);
    add(kj::fwd<decltype(codepoints)>(codepoints)...);
  }

  inline void add(UsvStringIterator it) { add(*it); }

  void addAll(UsvStringIterator begin, UsvStringIterator end);

  inline void addAll(UsvStringPtr other) { addAll(other.begin(), other.end()); }

  inline void addAll(kj::StringPtr str) { addAll(usv(str)); }

  inline void addAll(kj::ArrayPtr<char> sequence) { addAll(usv(sequence)); }

  inline void addAll(kj::ArrayPtr<uint16_t> sequence) { addAll(usv(sequence)); }

  inline void clear() { buffer.clear(); }

  inline void reserve(size_t size) { buffer.reserve(size); }

  inline void resize(size_t size) { buffer.resize(size); }

  inline size_t size() const { return buffer.size(); }

  inline bool empty() const { return size() == 0; }

  inline size_t capacity() const { return buffer.capacity(); }

  inline void truncate(size_t size) { buffer.truncate(size); }

  inline UsvString finish() KJ_WARN_UNUSED_RESULT  { return UsvString(buffer.releaseAsArray()); }

  inline kj::String finishAsStr() KJ_WARN_UNUSED_RESULT  { return finish().toStr(); }

  inline const kj::Vector<uint32_t>& storage() const KJ_LIFETIMEBOUND { return buffer; }

private:
  kj::Vector<uint32_t> buffer;
};

KJ_WARN_UNUSED_RESULT
UsvString usv(uint32_t codepoint, auto... codepoints) {
  UsvStringBuilder builder;
  builder.add(codepoint, kj::fwd<decltype(codepoints)>(codepoints)...);
  return builder.finish();
}

template <typename TypeWrapper>
class UsvStringWrapper {
public:
  static constexpr const char* getName(UsvString*) { return "string"; }
  static constexpr const char* getName(UsvStringPtr*) { return "string"; }

  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      UsvString string) {
    return v8Str(context->GetIsolate(), kj::mv(string));
  }

  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      UsvStringPtr string) {
    return v8Str(context->GetIsolate(), string);
  }

  kj::Maybe<UsvString> tryUnwrap(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      UsvString*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle.IsEmpty()) {
      return kj::none;
    }
    return usv(context->GetIsolate(), handle);
  }
};

kj::String KJ_STRINGIFY(const UsvString& string);
kj::String KJ_STRINGIFY(const UsvStringPtr& string);

kj::String KJ_STRINGIFY(UsvString& string);
kj::String KJ_STRINGIFY(UsvStringPtr& string);

}  // namespace workerd::jsg
