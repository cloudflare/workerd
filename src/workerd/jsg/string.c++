// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "string.h"
#include <unicode/utf8.h>
#include <unicode/utf16.h>
#include <algorithm>

namespace workerd::jsg {

namespace {
// In this variation, the result length will be <= buffer.size, with the exact
// size dependent on the number of paired or unpaired surrogates in the buffer.
kj::Array<uint32_t> transcodeToUtf32(kj::ArrayPtr<uint16_t> buffer) {
  if (buffer.size() == 0) {
    return kj::Array<uint32_t>();
  }
  kj::Vector<uint32_t> result(buffer.size());
  auto start = buffer.begin();
  auto offset = 0;
  while (offset < buffer.size()) {
    uint32_t codepoint;
    U16_NEXT_OR_FFFD(start, offset, buffer.size(), codepoint);
    result.add(codepoint);
  }

  return result.releaseAsArray();
}

// In this variation, we assume buffer is UTF8 encoded data. The result size
// will be <= buffer.
kj::Array<uint32_t> transcodeToUtf32(kj::ArrayPtr<const char> buffer) {
  if (buffer.size() == 0) {
    return kj::Array<uint32_t>();
  }
  kj::Vector<uint32_t> result(buffer.size());
  auto start = buffer.begin();
  auto offset = 0;
  while (offset < buffer.size()) {
    uint32_t codepoint;
    U8_NEXT_OR_FFFD(start, offset, buffer.size(), codepoint);
    result.add(codepoint);
  }

  return result.releaseAsArray();
}

auto transcodeToUtf8(const auto& buffer) {
  if (buffer.size() == 0) return kj::str();
  // In the worst case, we need four bytes per codepoint.
  kj::Vector<char> result(buffer.size() * 4);
  kj::byte token[4];

  for (auto codepoint : buffer) {
    auto offset = 0;
    U8_APPEND_UNSAFE(&token[0], offset, codepoint);
    for (auto n = 0; n < offset; n++) {
      result.add(token[n]);
    }
  }

  return kj::str(result.releaseAsArray());
}

auto transcodeToUtf16(const auto& buffer) {
  if (buffer.size() == 0) return kj::Array<uint16_t>();
  // Worst case, we need two uint16_t's per codepoint.
  kj::Vector<uint16_t> result(buffer.size() * 2);

  for (auto codepoint : buffer) {
    if (codepoint <= 0xffff) {
      result.add(static_cast<uint16_t>(codepoint));
    } else {
      result.add(static_cast<uint16_t>((codepoint >> 10)+0xd7c0));
      result.add(static_cast<uint16_t>((codepoint & 0x3ff)|0xdc00));
    }
  }

  return result.releaseAsArray();
}

kj::Array<uint32_t> writeFromV8String(v8::Isolate *isolate, v8::Local<v8::Value> value) {
  auto string = check(value->ToString(isolate->GetCurrentContext()));
  if (string->Length() == 0) return kj::Array<uint32_t>();

  auto buffer = kj::heapArray<uint16_t>(string->Length());
  string->Write(isolate, buffer.begin(), 0, -1, v8::String::NO_NULL_TERMINATION);

  return transcodeToUtf32(buffer);
}

kj::Array<uint32_t> copyUsvString(kj::ArrayPtr<uint32_t> other) {
  auto dest = kj::heapArray<uint32_t>(other.size());
  if (other.size() > 0) {
    memcpy(dest.begin(), other.begin(), other.size() * sizeof(uint32_t));
  }
  return kj::mv(dest);
}

kj::Maybe<size_t> findLastIndexOf(kj::ArrayPtr<uint32_t> buffer, uint32_t codepoint) {
  // kj::Array does not expose a regular iterator or reverse iterator so we
  // have to do this the manual way...
  size_t index = buffer.size();
  while (index != 0) {
    if (buffer[--index] == codepoint) return index;
  }
  return nullptr;
}

auto lexCmpThreeway(const kj::ArrayPtr<const uint32_t> one,
                    const kj::ArrayPtr<const uint32_t> two) {
  if (one.size() == 0 && two.size() == 0) return std::weak_ordering::equivalent;

  auto [left, right] = std::mismatch(
      one.begin(), one.end(),
      two.begin(), two.end());

  if (left == one.end() && right == two.end()) {
    return std::weak_ordering::equivalent;
  }

  return left == one.end() ? std::weak_ordering::less
       : right == two.end() ? std::weak_ordering::greater
       : *left <=> *right;
}
}  // namespace

UsvString usv(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  return UsvString(writeFromV8String(isolate, value));
}

UsvString usv(Lock& js, const jsg::JsValue& value) {
  return UsvString(writeFromV8String(js.v8Isolate, value));
}

UsvString usv(kj::ArrayPtr<uint16_t> string) {
  return UsvString(transcodeToUtf32(string));
}

UsvString usv(kj::ArrayPtr<const char> string) {
  return UsvString(transcodeToUtf32(string));
}

v8::Local<v8::String> v8Str(v8::Isolate* isolate, UsvStringPtr str, v8::NewStringType newType) {
  if (str.size() == 0) return v8::String::Empty(isolate);
  auto data = transcodeToUtf16(str.storage());
  return v8Str(isolate, data.asPtr(), newType);
}

uint32_t UsvStringIterator::operator*() const {
  KJ_REQUIRE(pos < size(), "Out-of-bounds read on UsvStringIterator.");
  return ptr[pos];
}

UsvStringIterator& UsvStringIterator::operator++() {
  KJ_REQUIRE(pos < size(), "Out-of-bounds increment on UsvStringIterator.");
  ++pos;
  return *this;
}

UsvStringIterator UsvStringIterator::operator+(int count) {
  KJ_REQUIRE(pos + count <= size(), "Out-of-bounds increment on UsvStringIterator.");
  UsvStringIterator iter = *this;
  iter += count;
  return iter;
}

UsvStringIterator UsvStringIterator::operator++(int) {
  auto iter = UsvStringIterator(*this);
  ++*this;
  return iter;
}

UsvStringIterator& UsvStringIterator::operator+=(int count) {
  KJ_REQUIRE(pos + count <= size(), "Out-of-bounds increment on UsvStringIterator.");
  pos += count;
  return *this;
}

UsvStringIterator& UsvStringIterator::operator--() {
  KJ_REQUIRE(pos > 0, "Out-of-bounds decrement on UsvStringIterator.");
  --pos;
  return *this;
}

UsvStringIterator UsvStringIterator::operator-(int count) {
  KJ_REQUIRE(count <= pos, "Out-of-bounds decrement on iterator.");
  UsvStringIterator iter = *this;
  iter -= count;
  return iter;
}

UsvStringIterator UsvStringIterator::operator--(int) {
  auto iter = UsvStringIterator(*this);
  --*this;
  return iter;
}

UsvStringIterator& UsvStringIterator::operator-=(int count) {
  KJ_REQUIRE(count <= pos, "Out-of-bounds decrement on UsvStringIterator.");
  pos -= count;
  return *this;
}

UsvString UsvString::clone() {
  return UsvString(copyUsvString(buffer));
}

uint32_t UsvString::getCodepointAt(size_t index) const {
  KJ_REQUIRE(index < size(), "Out-of-bounds read on UsvString.");
  return buffer[index];
}

UsvStringPtr UsvString::slice(size_t start, size_t end) {
  return UsvStringPtr(buffer.slice(start, end));
}

kj::String UsvString::toStr() {
  return transcodeToUtf8(buffer);
}

const kj::String UsvString::toStr() const {
  return transcodeToUtf8(buffer);
}

kj::Array<uint16_t> UsvString::toUtf16() {
  return transcodeToUtf16(buffer.asPtr());
}

const kj::Array<const uint16_t> UsvString::toUtf16() const {
  return transcodeToUtf16(buffer.asPtr());
}

UsvString UsvStringPtr::clone() {
  return UsvString(copyUsvString(ptr));
}

uint32_t UsvStringPtr::getCodepointAt(size_t index) const {
  KJ_REQUIRE(index < size(), "Out-of-bounds read on UsvStringPtr.");
  return ptr[index];
}

UsvStringPtr UsvStringPtr::slice(size_t start, size_t end) {
  return ptr.slice(start, end);
}

kj::String UsvStringPtr::toStr() {
  return transcodeToUtf8(ptr);
}

const kj::String UsvStringPtr::toStr() const {
  return transcodeToUtf8(ptr);
}

kj::Array<uint16_t> UsvStringPtr::toUtf16() {
  return transcodeToUtf16(ptr);
}

const kj::Array<const uint16_t> UsvStringPtr::toUtf16() const {
  return transcodeToUtf16(ptr);
}

void UsvStringBuilder::add(uint32_t codepoint) {
  KJ_REQUIRE(codepoint <= 0x10ffff, "Invalid Unicode codepoint.");
  buffer.add(codepoint);
}

void UsvStringBuilder::addAll(UsvStringIterator begin, UsvStringIterator end) {
  KJ_ASSERT(begin <= end, "Invalid iterator range.");
  while (begin < end) {
    add(*begin);
    ++begin;
  }
}

std::weak_ordering UsvString::operator<=>(const UsvString& other) const {
  return lexCmpThreeway(buffer, other.buffer);
}

std::weak_ordering UsvString::operator<=>(const UsvStringPtr& other) const {
  return lexCmpThreeway(buffer, other.ptr);
}

std::weak_ordering UsvString::operator<=>(UsvString& other) {
  return lexCmpThreeway(buffer, other.buffer);
}

std::weak_ordering UsvString::operator<=>(UsvStringPtr& other) {
  return lexCmpThreeway(buffer, other.ptr);
}

std::weak_ordering UsvStringPtr::operator<=>(const UsvString& other) const {
  return lexCmpThreeway(ptr, other.buffer);
}

std::weak_ordering UsvStringPtr::operator<=>(const UsvStringPtr& other) const {
  return lexCmpThreeway(ptr, other.ptr);
}

std::weak_ordering UsvStringPtr::operator<=>(UsvString& other) {
  return lexCmpThreeway(ptr, other.buffer);
}

std::weak_ordering UsvStringPtr::operator<=>(UsvStringPtr& other) {
  return lexCmpThreeway(ptr, other.ptr);
}

kj::Maybe<size_t> UsvString::lastIndexOf(uint32_t codepoint) {
  return findLastIndexOf(buffer, codepoint);
}

kj::Maybe<size_t> UsvStringPtr::lastIndexOf(uint32_t codepoint) {
  return findLastIndexOf(ptr, codepoint);
}

kj::String KJ_STRINGIFY(UsvString& string) {
  return string.toStr();
}

kj::String KJ_STRINGIFY(UsvStringPtr& string) {
  return string.toStr();
}

kj::String KJ_STRINGIFY(const UsvString& string) {
  return string.toStr();
}

kj::String KJ_STRINGIFY(const UsvStringPtr& string) {
  return string.toStr();
}

} // namespace workerd::jsg
