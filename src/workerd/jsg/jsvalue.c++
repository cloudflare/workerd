#include "jsvalue.h"

#include "buffersource.h"
#include "ser.h"
#include "simdutf.h"

#include <v8.h>

#include <kj/string-tree.h>
#include <kj/string.h>

#include <cmath>

namespace workerd::jsg {

JsValue::JsValue(v8::Local<v8::Value> inner): inner(inner) {
  requireOnStack(this);
}

bool JsValue::operator==(const JsValue& other) const {
  return inner == other.inner;
}

bool JsValue::strictEquals(const JsValue& other) const {
  return inner->StrictEquals(other.inner);
}

JsMap::operator JsObject() {
  return JsObject(inner);
}

void JsMap::set(Lock& js, const JsValue& name, const JsValue& value) {
  check(inner->Set(js.v8Context(), name.inner, value.inner));
}

void JsMap::set(Lock& js, kj::StringPtr name, const JsValue& value) {
  set(js, js.strIntern(name), value);
}

JsValue JsMap::get(Lock& js, const JsValue& name) {
  return JsValue(check(inner->Get(js.v8Context(), name.inner)));
}

JsValue JsMap::get(Lock& js, kj::StringPtr name) {
  return get(js, js.strIntern(name));
}

bool JsMap::has(Lock& js, const JsValue& name) {
  return check(inner->Has(js.v8Context(), name.inner));
}

bool JsMap::has(Lock& js, kj::StringPtr name) {
  return has(js, js.strIntern(name));
}

void JsMap::delete_(Lock& js, const JsValue& name) {
  check(inner->Delete(js.v8Context(), name.inner));
}

void JsMap::delete_(Lock& js, kj::StringPtr name) {
  delete_(js, js.strIntern(name));
}

void JsObject::defineProperty(Lock& js, kj::StringPtr name, const JsValue& value) {
  v8::Local<v8::String> nameStr = js.strIntern(name);
  check(inner->DefineOwnProperty(js.v8Context(), nameStr, value));
}

void JsObject::setReadOnly(Lock& js, kj::StringPtr name, const JsValue& value) {
  v8::Local<v8::String> nameStr = js.strIntern(name);
  check(inner->DefineOwnProperty(js.v8Context(), nameStr, value,
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete)));
}

void JsObject::setNonEnumerable(Lock& js, const JsSymbol& name, const JsValue& value) {
  check(inner->DefineOwnProperty(
      js.v8Context(), name.inner, value.inner, v8::PropertyAttribute::DontEnum));
}

void JsObject::setPrivate(Lock& js, kj::StringPtr name, const JsValue& value) {
  auto p = v8::Private::ForApi(js.v8Isolate, v8StrIntern(js.v8Isolate, name));
  check(inner->SetPrivate(js.v8Context(), p, value.inner));
}

JsValue JsObject::getPrivate(Lock& js, kj::StringPtr name) {
  auto p = v8::Private::ForApi(js.v8Isolate, v8StrIntern(js.v8Isolate, name));
  return JsValue(check(inner->GetPrivate(js.v8Context(), p)));
}

bool JsObject::hasPrivate(Lock& js, kj::StringPtr name) {
  auto p = v8::Private::ForApi(js.v8Isolate, v8StrIntern(js.v8Isolate, name));
  return check(inner->HasPrivate(js.v8Context(), p));
}

int JsObject::hashCode() const {
  return kj::hashCode(inner->GetIdentityHash());
}

kj::String JsObject::getConstructorName() {
  return kj::str(inner->GetConstructorName());
}

JsArray JsObject::getPropertyNames(Lock& js,
    KeyCollectionFilter keyFilter,
    PropertyFilter propertyFilter,
    IndexFilter indexFilter) {
  auto v8keyFilter = keyFilter == KeyCollectionFilter::INCLUDE_PROTOTYPES
      ? v8::KeyCollectionMode::kIncludePrototypes
      : v8::KeyCollectionMode::kOwnOnly;
  auto v8PropertyFilter = static_cast<v8::PropertyFilter>(propertyFilter);
  auto v8IndexFilter = indexFilter == IndexFilter::INCLUDE_INDICES
      ? v8::IndexFilter::kIncludeIndices
      : v8::IndexFilter::kSkipIndices;
  return JsArray(
      check(inner->GetPropertyNames(js.v8Context(), v8keyFilter, v8PropertyFilter, v8IndexFilter)));
}

JsArray JsObject::previewEntries(bool* isKeyValue) {
  return JsArray(check(inner->PreviewEntries(isKeyValue)));
}

void JsObject::recursivelyFreeze(Lock& js) {
  jsg::recursivelyFreeze(js.v8Context(), inner);
}

void JsObject::seal(Lock& js) {
  check(inner->SetIntegrityLevel(js.v8Context(), v8::IntegrityLevel::kSealed));
}

JsObject JsObject::jsonClone(Lock& js) {
  auto tmp = JsValue(inner).toJson(js);
  auto obj = KJ_ASSERT_NONNULL(JsValue::fromJson(js, tmp).tryCast<jsg::JsObject>());
  return JsObject(obj);
}

JsValue JsObject::getPrototype(Lock& js) {
  if (inner->IsProxy()) {
    // Here we emulate the behavior of v8's GetPrototypeV2() function for proxies.
    // If the proxy has a getPrototypeOf trap, we call it and return the result.
    // Otherwise we return the prototype of the target object.
    // Note that we do not check if the target object is extensible or not, or
    // if the returned prototype is consistent with the target's prototype if
    // the target is not extensible. See the comment below for more details.
    auto proxy = inner.As<v8::Proxy>();
    JSG_REQUIRE(!proxy->IsRevoked(), TypeError, "Proxy is revoked");
    auto handler = proxy->GetHandler();
    JSG_REQUIRE(handler->IsObject(), TypeError, "Proxy handler is not an object");
    auto jsHandler = JsObject(handler.As<v8::Object>());
    auto trap = jsHandler.get(js, "getPrototypeOf"_kj);
    auto target = proxy->GetTarget();
    if (trap.isUndefined()) {
      JSG_REQUIRE(target->IsObject(), TypeError, "Proxy target is not an object");
      // Run this through getPrototype to handle the case where the target is also a proxy.
      return JsObject(target.As<v8::Object>()).getPrototype(js);
    }
    JSG_REQUIRE(trap.isFunction(), TypeError, "Proxy getPrototypeOf trap is not a function");
    v8::Local<v8::Function> fn = ((v8::Local<v8::Value>)trap).As<v8::Function>();
    v8::Local<v8::Value> args[] = {target};
    auto ret = JsValue(check(fn->Call(js.v8Context(), jsHandler.inner, 1, args)));
    JSG_REQUIRE(ret.isObject() || ret.isNull(), TypeError,
        "Proxy getPrototypeOf trap did not return an object or null");
    // TODO(maybe): V8 performs additional checks on the returned value to
    // see if the proxy and the target are extensible or not, and if the
    // returned prototype is consistent with the target's prototype if they
    // are not extensible. To strictly match v8's behavior we should do the
    // same but (a) v8 does not expose the necessary APIs to do so, and (b)
    // it is not clear if we actually need to perform the additional check
    // given how we are currently using this function.
    return ret;
  }
  return JsValue(inner->GetPrototypeV2());
}

kj::String JsSymbol::description(Lock& js) const {
  auto desc = inner->Description(js.v8Isolate);
  if (desc.IsEmpty() || desc->IsUndefined()) {
    return kj::String();
  }
  return kj::str(desc);
}

void JsSet::add(Lock& js, const JsValue& value) {
  check(inner->Add(js.v8Context(), value.inner));
}

bool JsSet::has(Lock& js, const JsValue& value) const {
  return check(inner->Has(js.v8Context(), value.inner));
}

bool JsSet::delete_(Lock& js, const JsValue& value) {
  return check(inner->Delete(js.v8Context(), value.inner));
}

void JsSet::addAll(Lock& js, kj::ArrayPtr<const JsValue> values) {
  for (const JsValue& value: values) {
    check(inner->Add(js.v8Context(), value.inner));
  }
}

void JsSet::clear() {
  inner->Clear();
}

size_t JsSet::size() const {
  return inner->Size();
}

JsSet::operator JsArray() const {
  return JsArray(inner->AsArray());
}

kj::Maybe<int32_t> JsInt32::value(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  int32_t value;
  // The Int32Value(...) operation can fail with a JS exception, in which case
  // we return kj::none and the error should be allowed to propagate.
  if (inner->Int32Value(js.v8Context()).To(&value)) {
    return value;
  }
  return kj::none;
}

kj::Maybe<uint32_t> JsUint32::value(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  uint32_t value;
  // The Uint32Value(...) operation can fail with a JS exception, in which case
  // we return kj::none and the error should be allowed to propagate.
  if (inner->Uint32Value(js.v8Context()).To(&value)) {
    return value;
  }
  return kj::none;
};

kj::Maybe<int64_t> JsBigInt::toInt64(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  bool lossless = false;
  int64_t value = inner->Int64Value(&lossless);
  if (!lossless) {
    js.v8Isolate->ThrowException(js.rangeError("BigInt value does not fit in int64_t"));
    return kj::none;
  }
  return value;
}

kj::Maybe<uint64_t> JsBigInt::toUint64(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  bool lossless = false;
  uint64_t value = inner->Uint64Value(&lossless);
  if (!lossless) {
    js.v8Isolate->ThrowException(js.rangeError("BigInt value does not fit in uint64_t"));
    return kj::none;
  }
  return value;
}

kj::Maybe<double> JsNumber::value(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  double value;
  // The NumberValue(...) operation can fail with a JS exception, in which case
  // we return kj::none and the error should be allowed to propagate.
  if (inner->NumberValue(js.v8Context()).To(&value)) {
    return value;
  }
  return kj::none;
}

// ECMA-262, 15th edition, 21.1.2.5. Number.isSafeInteger
bool JsNumber::isSafeInteger(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  if (!inner->IsNumber()) return false;
  KJ_IF_SOME(value, value(js)) {
    if (std::isnan(value) || std::isinf(value) || std::trunc(value) != value) return false;
    constexpr uint64_t MAX_SAFE_INTEGER = (1ull << 53) - 1;
    if (std::abs(value) <= static_cast<double>(MAX_SAFE_INTEGER)) return true;
  }
  return false;
}

kj::Maybe<double> JsNumber::toSafeInteger(Lock& js) const {
  if (isSafeInteger(js)) {
    return inner.As<v8::Number>()->Value();
  }
  return kj::none;
}

bool JsValue::isTruthy(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  return inner->BooleanValue(js.v8Isolate);
}

kj::String JsValue::toString(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  return kj::str(inner);
}

JsString JsValue::toJsString(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  return JsString(check(inner->ToString(js.v8Context())));
}

kj::String JsValue::typeOf(Lock& js) const {
  KJ_ASSERT(!inner.IsEmpty());
  return kj::str(inner->TypeOf(js.v8Isolate));
}

#define V(Type)                                                                                    \
  bool JsValue::is##Type() const {                                                                 \
    return inner->Is##Type();                                                                      \
  }
JS_IS_TYPES(V)
#undef V

kj::String JsValue::toJson(Lock& js) const {
  return kj::str(check(v8::JSON::Stringify(js.v8Context(), inner)));
}

JsValue JsValue::fromJson(Lock& js, kj::ArrayPtr<const char> input) {
  return JsValue(check(v8::JSON::Parse(js.v8Context(), js.str(input))));
}

JsValue JsValue::fromJson(Lock& js, const JsValue& input) {
  return JsValue(check(v8::JSON::Parse(js.v8Context(), input.inner.As<v8::String>())));
}

bool JsBoolean::value(Lock& js) const {
  return inner->BooleanValue(js.v8Isolate);
}

uint32_t JsArray::size() const {
  return inner->Length();
}

JsValue JsArray::get(Lock& js, uint32_t i) const {
  return JsValue(check(inner->Get(js.v8Context(), i)));
}

void JsArray::add(Lock& js, const JsValue& value) {
  check(inner->Set(js.v8Context(), size(), value.inner));
}

JsArray::operator JsObject() const {
  return JsObject(inner.As<v8::Object>());
}

kj::String JsString::toString(jsg::Lock& js) const {
  auto buf = kj::heapArray<char>(inner->Utf8LengthV2(js.v8Isolate) + 1);
  inner->WriteUtf8V2(js.v8Isolate, buf.begin(), buf.size(), v8::String::WriteFlags::kNullTerminate);
  return kj::String(kj::mv(buf));
}

jsg::USVString JsString::toUSVString(Lock& js) const {
  auto buf = kj::heapArray<char>(inner->Utf8LengthV2(js.v8Isolate) + 1);
  inner->WriteUtf8V2(js.v8Isolate, buf.begin(), buf.size(),
      v8::String::WriteFlags::kNullTerminate | v8::String::WriteFlags::kReplaceInvalidUtf8);
  return jsg::USVString(kj::mv(buf));
}

jsg::ByteString JsString::toByteString(Lock& js) const {
  auto result = jsg::ByteString(toString(js));

  if (!simdutf::validate_ascii(result.begin(), result.size())) {
    // If storage is one-byte or the string contains only one-byte
    // characters, we know that it contains extended ASCII characters.
    //
    // The order of execution matters, since ContainsOnlyOneByte()
    // will scan the whole string for two-byte storage.
    if (inner->ContainsOnlyOneByte()) {
      result.warning = ByteString::Warning::CONTAINS_EXTENDED_ASCII;
    } else {
      // Storage is two-bytes and it contains two-byte characters.
      result.warning = ByteString::Warning::CONTAINS_UNICODE;
    }
  }

  return kj::mv(result);
}

jsg::DOMString JsString::toDOMString(Lock& js) const {
  auto buf = kj::heapArray<char>(inner->Utf8LengthV2(js.v8Isolate) + 1);
  inner->WriteUtf8V2(js.v8Isolate, buf.begin(), buf.size(), v8::String::WriteFlags::kNullTerminate);
  return jsg::DOMString(kj::mv(buf));
}

int JsString::hashCode() const {
  return kj::hashCode(inner->GetIdentityHash());
}

JsString JsString::concat(Lock& js, const JsString& one, const JsString& two) {
  return JsString(v8::String::Concat(js.v8Isolate, one.inner, two.inner));
}

bool JsString::operator==(const JsString& other) const {
  return inner->StringEquals(other.inner);
}

JsString JsString::internalize(Lock& js) const {
  return JsString(inner->InternalizeString(js.v8Isolate));
}

JsString::WriteIntoStatus JsString::writeInto(
    Lock& js, kj::ArrayPtr<char> buffer, WriteFlags options) const {
  WriteIntoStatus result = {0, 0};
  if (buffer.size() > 0) {
    result.written =
        inner->WriteUtf8V2(js.v8Isolate, buffer.begin(), buffer.size(), options, &result.read);
  }
  return result;
}

JsString::WriteIntoStatus JsString::writeInto(
    Lock& js, kj::ArrayPtr<uint16_t> buffer, WriteFlags options) const {
  WriteIntoStatus result = {0, 0};
  if (buffer.size() > 0) {
    result.written = kj::min(buffer.size(), length(js));
    inner->WriteV2(js.v8Isolate, 0, result.written, buffer.begin(), options);
    result.read = length(js);
  }
  return result;
}

JsString::WriteIntoStatus JsString::writeInto(
    Lock& js, kj::ArrayPtr<kj::byte> buffer, WriteFlags options) const {
  WriteIntoStatus result = {0, 0};
  if (buffer.size() > 0) {
    result.written = kj::min(buffer.size(), length(js));
    inner->WriteOneByteV2(
        js.v8Isolate, 0, kj::min(length(js), buffer.size()), buffer.begin(), options);
    result.read = length(js);
  }
  return result;
}

bool JsString::containsOnlyOneByte() const {
  return inner->ContainsOnlyOneByte();
}

bool JsString::isOneByte() const {
  return inner->IsOneByte();
}

JsUint8Array JsUint8Array::alloc(Lock& js, size_t length) {
  auto buffer = v8::ArrayBuffer::New(js.v8Isolate, length);
  return JsUint8Array(v8::Uint8Array::New(buffer, 0, length));
}

JsUint8Array JsUint8Array::slice(size_t start, size_t end) const {
  v8::Local<v8::Uint8Array> inner = *this;
  KJ_ASSERT(start <= end && end <= size());
  v8::Local<v8::Uint8Array> sliced =
      v8::Uint8Array::New(inner->Buffer(), inner->ByteOffset() + start, end - start);
  return JsUint8Array(sliced);
}

kj::Maybe<JsUint8Array> JsString::writeIntoUint8Array(
    Lock& js, SkipBailOutForTesting skipBailout) const {
  // We have to avoid flattening the string. We stick only to APIs that we know
  // will not trigger flattening. The key goal is to eliminate the additional
  // memory allocation and copying that happens when flattening occurs. This is
  // especially important for large strings when we are close to the isolate heap
  // limit as flattening can cause additional GC activity and memory pressure that
  // can thrash the GC. The APIs we use here are known not to trigger flattening.
  // We cannot avoid the allocation of the destination buffer for the UTF-8 bytes
  // but we can avoid the intermediate allocation of a contiguous UTF-16 buffer.

  // Threshold above which we always try incremental encoding to avoid flattening costs.
  // This is set fairly low (4KB) because:
  // * Rope strings are common even for medium-sized strings in SSR workloads
  // * Flattening cost exists even for smaller strings
  // * The incremental path has bail-out logic to avoid wasted allocations
  // Below this threshold, the overhead of incremental encoding outweighs the benefit.
  static constexpr size_t INCREMENTAL_THRESHOLD = 4 * 1024;
  size_t length = inner->Length();

  // The IsOneByte() check can quickly tell us if the string is one-byte but is prone to false
  // negatives. If it returns true, then awesome, we know the string is one-byte. However if it
  // returns false, we follow up with a linear scan using ContainsOnlyOneByte() to be sure.
  // Note that even if the string contains only one-byte characters, the UTF-8 worst-case length
  // can still be up to 2x the length because characters in the range 0x80-0xFF will be encoded as
  // two-byte UTF-8 sequences.
  size_t multiplier = inner->IsOneByte() || inner->ContainsOnlyOneByte() ? 2 : 3;
  // Estimate the actual UTF-8 length we'd likely need based on the multiplier.
  // For one-byte strings (multiplier=2): average between all-ASCII (1x) and extended-ASCII (2x).
  // For multi-byte strings (multiplier=3): assume mixed content averaging ~2x, since pure ASCII
  // would be 1x and worst-case multi-byte would be 3x. Most real-world strings with multi-byte
  // characters are a mix.
  size_t estimatedUtf8Length = multiplier == 2 ? (length * 3 / 2) : (length * 2);

  // Calculate the peak memory cost of the flattening path:
  // * UTF-16 temporary buffer: length * 2
  // * UTF-8 output buffer: estimatedUtf8Length
  // Both need to exist in heap memory simultaneously during encoding.
  size_t flattenPeakCost = (length * 2) + estimatedUtf8Length;

  // The worst-case UTF-8 buffer size needed for incremental encoding.
  size_t maxUtf8Length = length * multiplier;

  // If the string is already flat, the heap pressure is low, or the string is small,
  // we skip incremental encoding and let V8 handle its own way. Specifically, we
  // only need to take this path when the string is a rope and the heap pressure is
  // high
  if (!skipBailout) [[likely]] {
    if (inner->IsFlat() || js.getHeapPressure() < jsg::Lock::HeapPressure::APPROACHING ||
        length <= INCREMENTAL_THRESHOLD || maxUtf8Length >= flattenPeakCost) {
      return kj::none;
    }
  }

  // We will use an intermediate buffer to read chunks of the string into before encoding them
  // into UTF-8. This avoids flattening the string and allocating the full UTF-16 length in
  // memory but does require some additional processing that has its own overhead. We choose
  // the size of the intermediate buffer based on the size of the input string to balance
  // some of these trade-offs.
  //
  // Note that these thresholds are somewhat arbitrary and could likely be tuned further based
  // on real world workload.
  static constexpr size_t kLargeChunkThreshold = 2 * 1024 * 1024;  // 2 MB
  static constexpr size_t kMediumIntermediate = 4 * 4096;
  static constexpr size_t kLargeIntermediate = 2 * kMediumIntermediate;

  size_t chunkSize = length > kLargeChunkThreshold ? kLargeIntermediate : kMediumIntermediate;

  // If the string is <= kLargeChunkThreshold, then our intermediate buffer is stack
  // allocated. For larger strings, we allocate the intermediate buffer on the heap
  // and use a larger chunk size to reduce the number of iterations (at the cost of
  // wasting the fixed stack allocation).
  kj::SmallArray<char16_t, kMediumIntermediate> intermediate(chunkSize);
  kj::ArrayPtr<uint16_t> intermediateView(
      reinterpret_cast<uint16_t*>(intermediate.begin()), intermediate.size());

  // Use a growing destination vector to avoid worst-case allocation. This is the intermediate
  // vector that actually holds the UTF-8 output data. Start with our estimated size and grow
  // as needed. This is our key memory trade-off. We sacrifice c++ heap allocation to avoid
  // isolate heap allocation and the associated GC pressure it brings. In either case we have
  // to allocate the UTF-8 data somewhere.
  kj::Vector<kj::byte> output(estimatedUtf8Length);

  // The number of code units we have remaining to read from the string.
  size_t remaining = length;
  kj::Maybe<uint16_t> carryOverLeadSurrogate;

  while (remaining) {
    // If we have a carry-over lead surrogate from the previous iteration, we need to write it
    // into the intermediate buffer first.
    bool hadCarryOver = false;
    KJ_IF_SOME(lead, carryOverLeadSurrogate) {
      intermediateView[0] = lead;
      intermediateView = intermediateView.slice(1);
      hadCarryOver = true;
    }

    size_t toRead = kj::min(remaining, intermediateView.size());
    KJ_ASSERT(toRead > 0, "toRead must be greater than 0");

    size_t offset = length - remaining;

    // WriteV2 does not flatten the string. Yay!
    // TODO(later): This could probably be optimized further by using the one-byte variant
    // for one-byte strings but given that we should only get here rarely, that optimization
    // is not urgent.
    inner->WriteV2(
        js.v8Isolate, offset, toRead, intermediateView.begin(), v8::String::WriteFlags::kNone);

    // Let's check if the last code unit we read is a lead surrogate. If it is, we need to carry
    // it over to the next iteration so that we can properly encode the surrogate pair into UTF-8.
    uint16_t lastCodeUnit = intermediateView[toRead - 1];
    if (lastCodeUnit >= 0xD800 && lastCodeUnit <= 0xDBFF) {
      carryOverLeadSurrogate = lastCodeUnit;
      toRead--;
      remaining--;
    } else {
      carryOverLeadSurrogate = kj::none;
    }

    size_t actualRead = toRead + (hadCarryOver ? 1 : 0);

    // Calculate the exact UTF-8 length needed for this chunk.
    size_t chunkUtf8Length = simdutf::utf8_length_from_utf16(intermediate.begin(), actualRead);

    // Ensure we have space in the output vector (will grow if needed).
    size_t currentSize = output.size();
    output.resize(currentSize + chunkUtf8Length);

    // Encode the chunk directly into the output vector.
    size_t written = simdutf::convert_utf16_to_utf8_safe(intermediate.begin(), actualRead,
        reinterpret_cast<char*>(output.begin() + currentSize), chunkUtf8Length);

    KJ_ASSERT(written == chunkUtf8Length, "UTF-8 conversion wrote unexpected number of bytes");

    // Reset the intermediate view for the next iteration.
    intermediateView =
        kj::arrayPtr(reinterpret_cast<uint16_t*>(intermediate.begin()), intermediate.size());

    remaining -= toRead;
  }

  // Reading is done. Nothing should have caused the string to be flattened or we defeated
  // the purpose of taking this path.
  KJ_ASSERT(!inner->IsFlat() || skipBailout == SkipBailOutForTesting::YES);

  // Allocate the final Uint8Array in the heap with the exact size needed and copy the data.
  // This final copy is unavoidable since we are specifically trying to limit the memory usage
  // in the isolate heap by avoiding over-allocation. If we didn't copy here, we'd have to
  // allocate the full worst-case size up front which would defeat the purpose of this whole
  // exercise. We also have to copy because we're using the v8 sandbox, which requires backing
  // stores to be allocated in the heap.
  auto result = JsUint8Array::alloc(js, output.size());
  result.asArrayPtr().copyFrom(output);
  return result;
}

kj::Maybe<JsArray> JsRegExp::operator()(Lock& js, const JsString& input) const {
  auto result = check(inner->Exec(js.v8Context(), input));
  if (result->IsNullOrUndefined()) return kj::none;
  return JsArray(result.As<v8::Array>());
}

kj::Maybe<JsArray> JsRegExp::operator()(Lock& js, kj::StringPtr input) const {
  auto result = check(inner->Exec(js.v8Context(), js.str(input)));
  if (result->IsNull()) return kj::none;
  return JsArray(result.As<v8::Array>());
}

bool JsRegExp::match(Lock& js, kj::StringPtr input) {
  auto result = check(inner->Exec(js.v8Context(), js.str(input)));
  return !result->IsNull();
}

jsg::ByteString JsDate::toUTCString(jsg::Lock& js) const {
  JsString str(inner->ToUTCString());
  return jsg::ByteString(str.toString(js));
}

jsg::ByteString JsDate::toISOString(jsg::Lock& js) const {
  JsString str(inner->ToISOString());
  return jsg::ByteString(str.toString(js));
}

JsDate::operator kj::Date() const {
  return kj::UNIX_EPOCH + (int64_t(inner->ValueOf()) * kj::MILLISECONDS);
}

JsRegExp Lock::regexp(kj::StringPtr str, RegExpFlags flags, kj::Maybe<uint32_t> backtrackLimit) {
  KJ_IF_SOME(limit, backtrackLimit) {
    return JsRegExp(check(v8::RegExp::NewWithBacktrackLimit(
        v8Context(), v8Str(v8Isolate, str), static_cast<v8::RegExp::Flags>(flags), limit)));
  }
  return JsRegExp(check(
      v8::RegExp::New(v8Context(), v8Str(v8Isolate, str), static_cast<v8::RegExp::Flags>(flags))));
}

JsObject Lock::obj(kj::ArrayPtr<kj::StringPtr> keys, kj::ArrayPtr<JsValue> values) {
  KJ_DASSERT(keys.size() == values.size());
  v8::LocalVector<v8::Name> keys_(v8Isolate, keys.size());
  v8::LocalVector<v8::Value> values_(v8Isolate, keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    keys_[i] = strIntern(keys[i]).inner;
    values_[i] = values[i];
  }
  return JsObject(v8::Object::New(
      v8Isolate, v8::Object::New(v8Isolate), keys_.data(), values_.data(), keys.size()));
}

JsObject Lock::objNoProto(kj::ArrayPtr<kj::StringPtr> keys, kj::ArrayPtr<JsValue> values) {
  KJ_DASSERT(keys.size() == values.size());
  v8::LocalVector<v8::Name> keys_(v8Isolate, keys.size());
  v8::LocalVector<v8::Value> values_(v8Isolate, keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    keys_[i] = strIntern(keys[i]).inner;
    values_[i] = values[i];
  }
  return JsObject(
      v8::Object::New(v8Isolate, v8::Null(v8Isolate), keys_.data(), values_.data(), keys.size()));
}

JsArray Lock::arr(kj::ArrayPtr<JsValue> values) {
  v8::LocalVector<v8::Value> items(v8Isolate, values.size());
  for (size_t n = 0; n < values.size(); n++) {
    items[n] = values[n];
  }
  return JsArray(v8::Array::New(v8Isolate, items.data(), items.size()));
}

#define V(Name)                                                                                    \
  JsSymbol Lock::symbol##Name() {                                                                  \
    return JsSymbol(v8::Symbol::Get##Name(v8Isolate));                                             \
  }
JS_V8_SYMBOLS(V)
#undef V

JsDate Lock::date(kj::StringPtr date) {
  v8::Local<v8::Value> converted = check(v8::Date::Parse(v8Context(), str(date)));
  KJ_REQUIRE(converted->IsDate());
  return JsDate(converted.As<v8::Date>());
}

JsPromise Lock::rejectedJsPromise(jsg::JsValue exception) {
  v8::EscapableHandleScope handleScope(v8Isolate);
  auto context = v8Context();
  auto resolver = check(v8::Promise::Resolver::New(context));
  check(resolver->Reject(context, exception));
  return JsPromise(handleScope.Escape(resolver->GetPromise()));
}

JsPromise Lock::rejectedJsPromise(kj::Exception&& exception, ExceptionToJsOptions options) {
  return rejectedJsPromise(exceptionToJsValue(kj::mv(exception), options).getHandle(*this));
}

PromiseState JsPromise::state() {
  switch (inner->State()) {
    case v8::Promise::PromiseState::kPending:
      return PromiseState::PENDING;
    case v8::Promise::PromiseState::kFulfilled:
      return PromiseState::FULFILLED;
    case v8::Promise::PromiseState::kRejected:
      return PromiseState::REJECTED;
  }
  KJ_UNREACHABLE
}

JsValue JsPromise::result() {
  return JsValue(inner->Result());
}

JsValue JsProxy::target() {
  return JsValue(inner->GetTarget());
}

JsValue JsProxy::handler() {
  return JsValue(inner->GetHandler());
}

JsRef<JsValue> JsValue::addRef(Lock& js) {
  return JsRef<JsValue>(js, *this);
}

JsValue JsValue::structuredClone(Lock& js, kj::Maybe<kj::Array<JsValue>> maybeTransfers) {
  return jsg::structuredClone(js, *this, kj::mv(maybeTransfers));
}

JsMessage JsMessage::create(Lock& js, const JsValue& exception) {
  return JsMessage(v8::Exception::CreateMessage(js.v8Isolate, exception));
}

void JsMessage::addJsStackTrace(Lock& js, kj::Vector<kj::String>& lines) {
  if (inner.IsEmpty()) return;

  // TODO(someday): Relying on v8::Message to pass around source locations means
  // we can't provide the module name for errors like compiling wasm modules. We
  // should have our own type, but it requires a refactor of how we pass around errors
  // for script startup.

  static constexpr auto addLineCol = [](kj::StringTree str, int line, int col) {
    if (line != v8::Message::kNoLineNumberInfo) {
      str = kj::strTree(kj::mv(str), ":", line);
      if (col != v8::Message::kNoColumnInfo) {
        str = kj::strTree(kj::mv(str), ":", col);
      }
    }
    return str;
  };

  auto context = js.v8Context();
  auto trace = inner->GetStackTrace();
  if (trace.IsEmpty() || trace->GetFrameCount() == 0) {
    kj::StringTree locationStr;

    auto resourceNameVal = inner->GetScriptResourceName();
    if (resourceNameVal->IsString()) {
      auto resourceName = resourceNameVal.As<v8::String>();
      if (!resourceName.IsEmpty() && resourceName->Length() != 0) {
        locationStr = kj::strTree("  at ", resourceName);
      }
    }

    auto lineNumber = jsg::check(inner->GetLineNumber(context));
    auto columnNumber = jsg::check(inner->GetStartColumn(context));
    locationStr = addLineCol(kj::mv(locationStr), lineNumber, columnNumber);

    if (locationStr.size() > 0) {
      lines.add(locationStr.flatten());
    }
  } else {
    for (auto i: kj::zeroTo(trace->GetFrameCount())) {
      auto frame = trace->GetFrame(js.v8Isolate, i);
      kj::StringTree locationStr;

      auto scriptName = frame->GetScriptName();
      if (!scriptName.IsEmpty() && scriptName->Length() != 0) {
        locationStr = kj::strTree("  at ", scriptName);
      } else {
        locationStr = kj::strTree("  at worker.js");
      }

      auto lineNumber = frame->GetLineNumber();
      auto columnNumber = frame->GetColumn();
      locationStr = addLineCol(kj::mv(locationStr), lineNumber, columnNumber);

      auto func = frame->GetFunctionName();
      if (!func.IsEmpty() && func->Length() != 0) {
        locationStr = kj::strTree(kj::mv(locationStr), " in ", func);
      }

      lines.add(locationStr.flatten());
    }
  }
}

size_t JsFunction::length(Lock& js) const {
  JsObject obj = *this;
  auto lengthVal = obj.get(js, "length"_kj);
  KJ_IF_SOME(num, lengthVal.tryCast<jsg::JsNumber>()) {
    return static_cast<size_t>(num.value(js).orDefault(0));
  }
  return 0;
}

JsString JsFunction::name(Lock& js) const {
  JsObject obj = *this;
  auto nameVal = obj.get(js, "name"_kj);
  // It really shouldn't ever be possible for the name property to be non-string,
  // but just in case, we check and throw if that happens.
  return JSG_REQUIRE_NONNULL(
      nameVal.tryCast<jsg::JsString>(), TypeError, "Function name is not a string");
}

JsValue JsFunction::call(Lock& js, const JsValue& recv, v8::LocalVector<v8::Value>& args) const {
  v8::Local<v8::Function> fn = *this;
  return JsValue(check(fn->Call(js.v8Context(), recv, args.size(), args.data())));
}

JsValue JsFunction::callNoReceiver(Lock& js, v8::LocalVector<v8::Value>& args) const {
  return call(js, js.null(), args);
}

uint JsFunction::hashCode() const {
  v8::Local<v8::Function> obj = *this;
  return kj::hashCode(obj->GetIdentityHash());
}

BufferSource Lock::bytes(kj::Array<kj::byte> data) {
  return BufferSource(*this, BackingStore::from(*this, kj::mv(data)));
}

}  // namespace workerd::jsg
