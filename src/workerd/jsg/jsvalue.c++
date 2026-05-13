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
  return jsg::JsObject(inner);
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
  // Iteratively unwrap nested Proxy targets so that an attacker-controlled
  // chain of `new Proxy(prev, {})` cannot drive unbounded native recursion.
  // V8's own JSProxy::GetPrototype does the same and caps at kMaxIterationLimit.
  static constexpr int kMaxProxyDepth = 100'000;
  v8::Local<v8::Object> current = inner;
  for (int depth = 0; current->IsProxy(); ++depth) {
    JSG_REQUIRE(
        depth < kMaxProxyDepth, RangeError, "Maximum proxy chain length exceeded in getPrototype");
    auto proxy = current.As<v8::Proxy>();
    JSG_REQUIRE(!proxy->IsRevoked(), TypeError, "Proxy is revoked");
    auto handler = proxy->GetHandler();
    JSG_REQUIRE(handler->IsObject(), TypeError, "Proxy handler is not an object");
    auto jsHandler = JsObject(handler.As<v8::Object>());
    auto trap = jsHandler.get(js, "getPrototypeOf"_kj);
    auto target = proxy->GetTarget();
    if (trap.isUndefined()) {
      JSG_REQUIRE(target->IsObject(), TypeError, "Proxy target is not an object");
      current = target.As<v8::Object>();
      continue;  // unwrap one layer iteratively, no native recursion
    }
    JSG_REQUIRE(trap.isFunction(), TypeError, "Proxy getPrototypeOf trap is not a function");
    v8::Local<v8::Function> fn = (v8::Local<v8::Value>(trap)).As<v8::Function>();
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
#if V8_MAJOR_VERSION >= 15 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 7)
  return JsValue(current->GetPrototype());
#else
  // TODO(cleanup): Remove when unnecessary.
  return JsValue(current->GetPrototypeV2());
#endif
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
  return jsg::JsArray(inner->AsArray());
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
  return jsg::JsObject(inner.As<v8::Object>());
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

bool JsString::isFlat() const {
  return inner->IsFlat();
}

bool JsString::containsOnlyOneByte() const {
  return inner->ContainsOnlyOneByte();
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

kj::String JsDate::toUTCString(jsg::Lock& js) const {
  JsString str(inner->ToUTCString());
  return str.toString(js);
}

kj::String JsDate::toISOString(jsg::Lock& js) const {
  JsString str(inner->ToISOString());
  return str.toString(js);
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

JsObject Lock::obj(kj::ArrayPtr<const kj::StringPtr> keys, kj::ArrayPtr<JsValue> values) {
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

JsPromise Lock::resolvedJsPromise(jsg::JsValue value) {
  v8::EscapableHandleScope handleScope(v8Isolate);
  auto context = v8Context();
  auto resolver = check(v8::Promise::Resolver::New(context));
  check(resolver->Resolve(context, value));
  return JsPromise(handleScope.Escape(resolver->GetPromise()));
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

JsUint8Array Lock::bytes(kj::ArrayPtr<const kj::byte> data) {
  return JsUint8Array::create(*this, data);
}

JsArrayBuffer Lock::arrayBuffer(kj::ArrayPtr<const kj::byte> data) {
  return JsArrayBuffer::create(*this, data);
}

// ======================================================================================
// JsArrayBuffer

kj::Maybe<JsArrayBuffer> JsArrayBuffer::tryCreate(Lock& js, size_t length) {
  JSG_REQUIRE(length < v8::ArrayBuffer::kMaxByteLength, RangeError, "The length is too large");
  auto backing = v8::ArrayBuffer::NewBackingStore(js.v8Isolate, length,
      v8::BackingStoreInitializationMode::kZeroInitialized,
      v8::BackingStoreOnFailureMode::kReturnNull);
  if (backing == nullptr) return kj::none;
  return create(js, kj::mv(backing));
}

JsArrayBuffer JsArrayBuffer::create(Lock& js, size_t length) {
  JSG_REQUIRE(length < v8::ArrayBuffer::kMaxByteLength, RangeError, "The length is too large");
  auto backing = v8::ArrayBuffer::NewBackingStore(js.v8Isolate, length,
      v8::BackingStoreInitializationMode::kZeroInitialized,
      v8::BackingStoreOnFailureMode::kReturnNull);
  JSG_REQUIRE(backing != nullptr, RangeError, "Failed to allocate memory for ArrayBuffer");
  return create(js, kj::mv(backing));
}

JsArrayBuffer JsArrayBuffer::create(Lock& js, kj::ArrayPtr<const kj::byte> data) {
  auto buf = create(js, data.size());
  buf.asArrayPtr().copyFrom(data);
  return buf;
}

JsArrayBuffer JsArrayBuffer::create(Lock& js, std::unique_ptr<v8::BackingStore> backingStore) {
  return JsArrayBuffer(v8::ArrayBuffer::New(js.v8Isolate, kj::mv(backingStore)));
}

JsArrayBuffer JsArrayBuffer::create(Lock& js, std::shared_ptr<v8::BackingStore> backingStore) {
  return JsArrayBuffer(v8::ArrayBuffer::New(js.v8Isolate, kj::mv(backingStore)));
}

kj::ArrayPtr<kj::byte> JsArrayBuffer::asArrayPtr() {
  v8::Local<v8::ArrayBuffer> inner = *this;
  if (inner->WasDetached()) [[unlikely]] {
    return nullptr;
  }
  void* data = inner->GetBackingStore()->Data();
  size_t length = inner->ByteLength();
  return kj::ArrayPtr(static_cast<kj::byte*>(data), length);
}

kj::ArrayPtr<const kj::byte> JsArrayBuffer::asArrayPtr() const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  if (inner->WasDetached()) [[unlikely]] {
    return nullptr;
  }
  const void* data = inner->GetBackingStore()->Data();
  size_t length = inner->ByteLength();
  return kj::ArrayPtr(static_cast<const kj::byte*>(data), length);
}

JsArrayBuffer JsArrayBuffer::slice(Lock& js, size_t newLength) const {
  JSG_REQUIRE(newLength <= size(), RangeError, "New length exceeds buffer length");
  auto dest = create(js, newLength);
  dest.asArrayPtr().copyFrom(asArrayPtr().slice(0, newLength));
  return dest;
}

size_t JsArrayBuffer::size() const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return inner->ByteLength();
}

kj::Array<kj::byte> JsArrayBuffer::copy() {
  auto ptr = asArrayPtr();
  return kj::heapArray(ptr);
}

JsArrayBuffer::operator JsBufferSource() const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return jsg::JsBufferSource(inner);
}

bool JsArrayBuffer::isDetachable() const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return inner->IsDetachable();
}

bool JsArrayBuffer::isDetached() const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return inner->WasDetached();
}

void JsArrayBuffer::detachInPlace(Lock& js) {
  JSG_REQUIRE(isDetachable(), TypeError, "ArrayBuffer is not detachable");
  v8::Local<v8::ArrayBuffer> inner = *this;
  check(inner->Detach({}));
}

JsArrayBuffer JsArrayBuffer::detachAndTake(Lock& js) {
  JSG_REQUIRE(isDetachable(), TypeError, "ArrayBuffer is not detachable");
  v8::Local<v8::ArrayBuffer> inner = *this;
  auto backing = inner->GetBackingStore();
  check(inner->Detach({}));
  return JsArrayBuffer(v8::ArrayBuffer::New(js.v8Isolate, kj::mv(backing)));
}

JsUint8Array JsArrayBuffer::newUint8View(size_t offset, size_t numElements) const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsUint8Array(v8::Uint8Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newInt8View(size_t offset, size_t numElements) const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Int8Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newUint8ClampedView(size_t offset, size_t numElements) const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Uint8ClampedArray::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newUint16View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 2 == 0, TypeError, "ArrayBuffer size is not a multiple of 2");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Uint16Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newInt16View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 2 == 0, TypeError, "ArrayBuffer size is not a multiple of 2");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Int16Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newUint32View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 4 == 0, TypeError, "ArrayBuffer size is not a multiple of 4");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Uint32Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newInt32View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 4 == 0, TypeError, "ArrayBuffer size is not a multiple of 4");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Int32Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newFloat16View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 2 == 0, TypeError, "ArrayBuffer size is not a multiple of 2");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Float16Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newFloat32View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 4 == 0, TypeError, "ArrayBuffer size is not a multiple of 4");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Float32Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newFloat64View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 8 == 0, TypeError, "ArrayBuffer size is not a multiple of 8");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Float64Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newBigInt64View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 8 == 0, TypeError, "ArrayBuffer size is not a multiple of 8");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::BigInt64Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newBigUint64View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 8 == 0, TypeError, "ArrayBuffer size is not a multiple of 8");
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::BigUint64Array::New(inner, offset, numElements));
}
JsArrayBufferView JsArrayBuffer::newDataView(size_t offset, size_t numElements) const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::DataView::New(inner, offset, numElements));
}

bool JsArrayBuffer::isResizable() const {
  v8::Local<v8::ArrayBuffer> inner = *this;
  return inner->IsResizableByUserJavaScript();
}

JsArrayBuffer::operator JsUint8Array() const {
  return newUint8View(0, size());
}

// ======================================================================================
// JsSharedArrayBuffer

kj::Maybe<JsSharedArrayBuffer> JsSharedArrayBuffer::tryCreate(Lock& js, size_t length) {
  JSG_REQUIRE(length < v8::ArrayBuffer::kMaxByteLength, RangeError, "The length is too large");
  auto backing = v8::SharedArrayBuffer::NewBackingStore(js.v8Isolate, length,
      v8::BackingStoreInitializationMode::kZeroInitialized,
      v8::BackingStoreOnFailureMode::kReturnNull);
  if (backing == nullptr) return kj::none;
  return create(js, kj::mv(backing));
}

JsSharedArrayBuffer JsSharedArrayBuffer::create(Lock& js, size_t length) {
  JSG_REQUIRE(length < v8::ArrayBuffer::kMaxByteLength, RangeError, "The length is too large");
  auto backing = v8::SharedArrayBuffer::NewBackingStore(js.v8Isolate, length,
      v8::BackingStoreInitializationMode::kZeroInitialized,
      v8::BackingStoreOnFailureMode::kReturnNull);
  JSG_REQUIRE(backing != nullptr, RangeError, "Failed to allocate memory for ArrayBuffer");
  return create(js, kj::mv(backing));
}

JsSharedArrayBuffer JsSharedArrayBuffer::create(Lock& js, kj::ArrayPtr<const kj::byte> data) {
  auto buf = create(js, data.size());
  buf.asArrayPtr().copyFrom(data);
  return buf;
}

JsSharedArrayBuffer JsSharedArrayBuffer::create(
    Lock& js, std::unique_ptr<v8::BackingStore> backingStore) {
  return JsSharedArrayBuffer(v8::SharedArrayBuffer::New(js.v8Isolate, kj::mv(backingStore)));
}

JsSharedArrayBuffer JsSharedArrayBuffer::create(
    Lock& js, std::shared_ptr<v8::BackingStore> backingStore) {
  return JsSharedArrayBuffer(v8::SharedArrayBuffer::New(js.v8Isolate, kj::mv(backingStore)));
}

kj::ArrayPtr<kj::byte> JsSharedArrayBuffer::asArrayPtr() {
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  void* data = inner->GetBackingStore()->Data();
  size_t length = inner->ByteLength();
  return kj::ArrayPtr(static_cast<kj::byte*>(data), length);
}

kj::ArrayPtr<const kj::byte> JsSharedArrayBuffer::asArrayPtr() const {
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  const void* data = inner->GetBackingStore()->Data();
  size_t length = inner->ByteLength();
  return kj::ArrayPtr(static_cast<const kj::byte*>(data), length);
}

JsSharedArrayBuffer JsSharedArrayBuffer::slice(Lock& js, size_t newLength) const {
  JSG_REQUIRE(newLength <= size(), RangeError, "New length exceeds buffer length");
  auto dest = create(js, newLength);
  dest.asArrayPtr().copyFrom(asArrayPtr().slice(0, newLength));
  return dest;
}

size_t JsSharedArrayBuffer::size() const {
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return inner->ByteLength();
}

kj::Array<kj::byte> JsSharedArrayBuffer::copy() {
  auto ptr = asArrayPtr();
  return kj::heapArray(ptr);
}

JsSharedArrayBuffer::operator JsBufferSource() const {
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return jsg::JsBufferSource(inner);
}

JsUint8Array JsSharedArrayBuffer::newUint8View(size_t offset, size_t numElements) const {
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsUint8Array(v8::Uint8Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newInt8View(size_t offset, size_t numElements) const {
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Int8Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newUint8ClampedView(
    size_t offset, size_t numElements) const {
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Uint8ClampedArray::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newUint16View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 2 == 0, TypeError, "ArrayBuffer size is not a multiple of 2");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Uint16Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newInt16View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 2 == 0, TypeError, "ArrayBuffer size is not a multiple of 2");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Int16Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newUint32View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 4 == 0, TypeError, "ArrayBuffer size is not a multiple of 4");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Uint32Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newInt32View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 4 == 0, TypeError, "ArrayBuffer size is not a multiple of 4");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Int32Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newFloat16View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 2 == 0, TypeError, "ArrayBuffer size is not a multiple of 2");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Float16Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newFloat32View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 4 == 0, TypeError, "ArrayBuffer size is not a multiple of 4");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Float32Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newFloat64View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 8 == 0, TypeError, "ArrayBuffer size is not a multiple of 8");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::Float64Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newBigInt64View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 8 == 0, TypeError, "ArrayBuffer size is not a multiple of 8");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::BigInt64Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newBigUint64View(size_t offset, size_t numElements) const {
  JSG_REQUIRE(size() % 8 == 0, TypeError, "ArrayBuffer size is not a multiple of 8");
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::BigUint64Array::New(inner, offset, numElements));
}
JsArrayBufferView JsSharedArrayBuffer::newDataView(size_t offset, size_t numElements) const {
  v8::Local<v8::SharedArrayBuffer> inner = *this;
  return JsArrayBufferView(v8::DataView::New(inner, offset, numElements));
}

JsSharedArrayBuffer::operator JsUint8Array() const {
  return newUint8View(0, size());
}

// ======================================================================================
// JsArrayBufferView

size_t JsArrayBufferView::size() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->ByteLength();
}

size_t JsArrayBufferView::getOffset() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->ByteOffset();
}

bool JsArrayBufferView::isIntegerType() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsUint8Array() || inner->IsUint8ClampedArray() || inner->IsInt8Array() ||
      inner->IsUint16Array() || inner->IsInt16Array() || inner->IsUint32Array() ||
      inner->IsInt32Array() || inner->IsBigInt64Array() || inner->IsBigUint64Array();
}

bool JsArrayBufferView::isUint8Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsUint8Array();
}

bool JsArrayBufferView::isInt8Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsInt8Array();
}

bool JsArrayBufferView::isUint8ClampedArray() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsUint8ClampedArray();
}

bool JsArrayBufferView::isUint16Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsUint16Array();
}

bool JsArrayBufferView::isInt16Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsInt16Array();
}

bool JsArrayBufferView::isUint32Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsUint32Array();
}

bool JsArrayBufferView::isInt32Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsInt32Array();
}

bool JsArrayBufferView::isFloat16Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsFloat16Array();
}

bool JsArrayBufferView::isFloat32Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsFloat32Array();
}

bool JsArrayBufferView::isFloat64Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsFloat64Array();
}

bool JsArrayBufferView::isBigInt64Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsBigInt64Array();
}

bool JsArrayBufferView::isBigUint64Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsBigUint64Array();
}

bool JsArrayBufferView::isDataView() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->IsDataView();
}

size_t JsArrayBufferView::getElementSize() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  if (inner->IsUint8Array() || inner->IsInt8Array() || inner->IsUint8ClampedArray()) {
    return 1;
  } else if (inner->IsUint16Array() || inner->IsInt16Array() || inner->IsFloat16Array()) {
    return 2;
  } else if (inner->IsUint32Array() || inner->IsInt32Array() || inner->IsFloat32Array()) {
    return 4;
  } else if (inner->IsFloat64Array() || inner->IsBigInt64Array() || inner->IsBigUint64Array()) {
    return 8;
  } else if (inner->IsDataView()) {
    return 1;  // DataView is byte-addressable
  }
  KJ_UNREACHABLE;  // Not a valid ArrayBufferView type
}

JsArrayBuffer JsArrayBufferView::getBuffer() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return JsArrayBuffer(inner->Buffer());
}

bool JsArrayBufferView::isDetachable() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->Buffer()->IsDetachable();
}

bool JsArrayBufferView::isDetached() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->Buffer()->WasDetached();
}

void JsArrayBufferView::detachInPlace(Lock& js) {
  v8::Local<v8::ArrayBufferView> inner = *this;
  check(inner->Buffer()->Detach({}));
}

JsArrayBufferView JsArrayBufferView::detachAndTake(Lock& js) {
  v8::Local<v8::ArrayBufferView> inner = *this;
  auto length = inner->ByteLength();
  auto offset = inner->ByteOffset();
  auto ab = getBuffer().detachAndTake(js);

  // We have to return the same type of vie
  if (inner->IsUint8Array()) {
    return ab.newUint8View(offset, length);
  } else if (inner->IsInt8Array()) {
    return ab.newInt8View(offset, length);
  } else if (inner->IsUint8ClampedArray()) {
    return ab.newUint8ClampedView(offset, length);
  } else if (inner->IsUint16Array()) {
    return ab.newUint16View(offset, length / getElementSize());
  } else if (inner->IsInt16Array()) {
    return ab.newInt16View(offset, length / getElementSize());
  } else if (inner->IsUint32Array()) {
    return ab.newUint32View(offset, length / getElementSize());
  } else if (inner->IsInt32Array()) {
    return ab.newInt32View(offset, length / getElementSize());
  } else if (inner->IsFloat16Array()) {
    return ab.newFloat16View(offset, length / getElementSize());
  } else if (inner->IsFloat32Array()) {
    return ab.newFloat32View(offset, length / getElementSize());
  } else if (inner->IsFloat64Array()) {
    return ab.newFloat64View(offset, length / getElementSize());
  } else if (inner->IsBigInt64Array()) {
    return ab.newBigInt64View(offset, length / getElementSize());
  } else if (inner->IsBigUint64Array()) {
    return ab.newBigUint64View(offset, length / getElementSize());
  } else if (inner->IsDataView()) {
    return ab.newDataView(offset, length);
  }

  KJ_UNREACHABLE;
}

JsArrayBufferView JsArrayBufferView::slice(Lock& js, size_t offset, size_t length) const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  offset = inner->ByteOffset() + offset;

  if (inner->IsUint8Array()) {
    return JsArrayBufferView(v8::Uint8Array::New(inner->Buffer(), offset, length));
  } else if (inner->IsInt8Array()) {
    return JsArrayBufferView(v8::Int8Array::New(inner->Buffer(), offset, length));
  } else if (inner->IsUint8ClampedArray()) {
    return JsArrayBufferView(v8::Uint8ClampedArray::New(inner->Buffer(), offset, length));
  } else if (inner->IsUint16Array()) {
    return JsArrayBufferView(
        v8::Uint16Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsInt16Array()) {
    return JsArrayBufferView(
        v8::Int16Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsUint32Array()) {
    return JsArrayBufferView(
        v8::Uint32Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsInt32Array()) {
    return JsArrayBufferView(
        v8::Int32Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsFloat16Array()) {
    return JsArrayBufferView(
        v8::Float16Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsFloat32Array()) {
    return JsArrayBufferView(
        v8::Float32Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsFloat64Array()) {
    return JsArrayBufferView(
        v8::Float64Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsBigInt64Array()) {
    return JsArrayBufferView(
        v8::BigInt64Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsBigUint64Array()) {
    return JsArrayBufferView(
        v8::BigUint64Array::New(inner->Buffer(), offset, length / getElementSize()));
  } else if (inner->IsDataView()) {
    return JsArrayBufferView(v8::DataView::New(inner->Buffer(), offset, length));
  }

  KJ_UNREACHABLE;
}

bool JsArrayBufferView::isResizable() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return inner->Buffer()->IsResizableByUserJavaScript();
}

JsArrayBufferView::operator JsBufferSource() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  return jsg::JsBufferSource(inner);
}

JsArrayBufferView::operator JsUint8Array() const {
  v8::Local<v8::ArrayBufferView> inner = *this;
  if (inner->IsUint8Array()) {
    return jsg::JsUint8Array(inner.As<v8::Uint8Array>());
  }

  auto buf = inner->Buffer();
  return jsg::JsUint8Array(v8::Uint8Array::New(buf, inner->ByteOffset(), inner->ByteLength()));
}

JsArrayBufferView JsArrayBufferView::clone(jsg::Lock& js) {
  v8::Local<v8::ArrayBufferView> inner = *this;
  auto backing = inner->Buffer()->GetBackingStore();
  auto ab = jsg::JsArrayBuffer::create(js, kj::mv(backing));

  auto offset = getOffset();
  auto length = size();

  if (inner->IsUint8Array()) {
    return ab.newUint8View(offset, length);
  } else if (inner->IsInt8Array()) {
    return ab.newInt8View(offset, length / getElementSize());
  } else if (inner->IsUint8ClampedArray()) {
    return ab.newUint8ClampedView(offset, length / getElementSize());
  } else if (inner->IsUint16Array()) {
    return ab.newUint16View(offset, length / getElementSize());
  } else if (inner->IsInt16Array()) {
    return ab.newInt16View(offset, length / getElementSize());
  } else if (inner->IsUint32Array()) {
    return ab.newUint32View(offset, length / getElementSize());
  } else if (inner->IsInt32Array()) {
    return ab.newInt32View(offset, length / getElementSize());
  } else if (inner->IsFloat16Array()) {
    return ab.newFloat16View(offset, length / getElementSize());
  } else if (inner->IsFloat32Array()) {
    return ab.newFloat32View(offset, length / getElementSize());
  } else if (inner->IsFloat64Array()) {
    return ab.newFloat64View(offset, length / getElementSize());
  } else if (inner->IsBigInt64Array()) {
    return ab.newBigInt64View(offset, length / getElementSize());
  } else if (inner->IsBigUint64Array()) {
    return ab.newBigUint64View(offset, length / getElementSize());
  } else if (inner->IsDataView()) {
    return ab.newDataView(offset, length);
  }
  KJ_UNREACHABLE;
}

// ======================================================================================
// JsBufferSource

kj::ArrayPtr<kj::byte> JsBufferSource::asArrayPtr() {
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    auto buf = inner.As<v8::ArrayBuffer>();
    if (buf->WasDetached()) [[unlikely]] {
      return nullptr;
    }
    return kj::ArrayPtr(static_cast<kj::byte*>(buf->Data()), buf->ByteLength());
  } else if (inner->IsSharedArrayBuffer()) {
    auto buf = inner.As<v8::SharedArrayBuffer>();
    return kj::ArrayPtr(static_cast<kj::byte*>(buf->Data()), buf->ByteLength());
  } else {
    KJ_DASSERT(inner->IsArrayBufferView());
    auto view = inner.As<v8::ArrayBufferView>();
    auto buf = view->Buffer();
    if (buf->WasDetached()) [[unlikely]] {
      return nullptr;
    }
    kj::byte* data = static_cast<kj::byte*>(buf->Data()) + view->ByteOffset();
    return kj::ArrayPtr(data, view->ByteLength());
  }
}

size_t JsBufferSource::size() const {
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    auto buf = inner.As<v8::ArrayBuffer>();
    if (buf->WasDetached()) [[unlikely]] {
      return 0;
    }
    return buf->ByteLength();
  } else if (inner->IsSharedArrayBuffer()) {
    auto buf = inner.As<v8::SharedArrayBuffer>();
    return buf->ByteLength();
  } else {
    KJ_DASSERT(inner->IsArrayBufferView());
    auto view = inner.As<v8::ArrayBufferView>();
    if (view->Buffer()->WasDetached()) [[unlikely]] {
      return 0;
    }
    return view->ByteLength();
  }
}

bool JsBufferSource::isIntegerType() const {
  v8::Local<v8::Value> inner = *this;
  return inner->IsArrayBuffer() || inner->IsSharedArrayBuffer() || inner->IsUint8Array() ||
      inner->IsUint8ClampedArray() || inner->IsInt8Array() || inner->IsUint16Array() ||
      inner->IsInt16Array() || inner->IsUint32Array() || inner->IsInt32Array() ||
      inner->IsBigInt64Array() || inner->IsBigUint64Array();
}

bool JsBufferSource::isSharedArrayBuffer() const {
  v8::Local<v8::Value> inner = *this;
  return inner->IsSharedArrayBuffer();
}

bool JsBufferSource::isArrayBuffer() const {
  v8::Local<v8::Value> inner = *this;
  return inner->IsArrayBuffer();
}

bool JsBufferSource::isArrayBufferView() const {
  v8::Local<v8::Value> inner = *this;
  return inner->IsArrayBufferView();
}

kj::Array<kj::byte> JsBufferSource::copy() {
  auto ptr = asArrayPtr();
  return kj::heapArray(ptr);
}

bool JsBufferSource::isResizable() const {
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    return inner.As<v8::ArrayBuffer>()->IsResizableByUserJavaScript();
  } else if (inner->IsArrayBufferView()) {
    return inner.As<v8::ArrayBufferView>()->Buffer()->IsResizableByUserJavaScript();
  }
  return false;
}

bool JsBufferSource::isDetachable() const {
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    return inner.As<v8::ArrayBuffer>()->IsDetachable();
  } else if (inner->IsSharedArrayBuffer()) {
    return false;  // SharedArrayBuffers are never detachable
  } else {
    KJ_DASSERT(inner->IsArrayBufferView());
    return inner.As<v8::ArrayBufferView>()->Buffer()->IsDetachable();
  }
}

bool JsBufferSource::isDetached() const {
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    return inner.As<v8::ArrayBuffer>()->WasDetached();
  } else if (inner->IsSharedArrayBuffer()) {
    return false;  // SharedArrayBuffers are never detachable
  } else {
    KJ_DASSERT(inner->IsArrayBufferView());
    return inner.As<v8::ArrayBufferView>()->Buffer()->WasDetached();
  }
}

void JsBufferSource::detachInPlace(Lock& js) {
  JSG_REQUIRE(isDetachable(), TypeError, "BufferSource is not detachable");
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    auto buf = inner.As<v8::ArrayBuffer>();
    check(buf->Detach({}));
  } else if (inner->IsSharedArrayBuffer()) {
    KJ_UNREACHABLE;  // SharedArrayBuffers are never detachable
  } else {
    KJ_DASSERT(inner->IsArrayBufferView());
    auto view = inner.As<v8::ArrayBufferView>();
    check(view->Buffer()->Detach({}));
  }
}

JsBufferSource JsBufferSource::detachAndTake(Lock& js) {
  JSG_REQUIRE(isDetachable(), TypeError, "BufferSource is not detachable");
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    JsArrayBuffer ab(inner.As<v8::ArrayBuffer>());
    return ab.detachAndTake(js);
  } else if (inner->IsSharedArrayBuffer()) {
    KJ_UNREACHABLE;  // SharedArrayBuffers are never detachable
  }

  KJ_DASSERT(inner->IsArrayBufferView());
  JsArrayBufferView view(inner.As<v8::ArrayBufferView>());
  return view.detachAndTake(js);
}

JsBufferSource::operator JsUint8Array() const {
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    JsArrayBuffer ab(inner.As<v8::ArrayBuffer>());
    return ab;
  }
  if (inner->IsSharedArrayBuffer()) {
    JsSharedArrayBuffer ab(inner.As<v8::SharedArrayBuffer>());
    return ab;
  }
  if (inner->IsUint8Array()) {
    return jsg::JsUint8Array(inner.As<v8::Uint8Array>());
  }
  JsArrayBufferView view(inner.As<v8::ArrayBufferView>());
  return view;
}

size_t JsBufferSource::getOffset() const {
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer() || inner->IsSharedArrayBuffer()) {
    return 0;
  }
  KJ_DASSERT(inner->IsArrayBufferView());
  auto view = inner.As<v8::ArrayBufferView>();
  return view->ByteOffset();
}

size_t JsBufferSource::underlyingArrayBufferSize(Lock& js) const {
  v8::Local<v8::Value> inner = *this;
  if (inner->IsArrayBuffer()) {
    auto buf = inner.As<v8::ArrayBuffer>();
    if (buf->WasDetached()) [[unlikely]] {
      return 0;
    }
    return buf->ByteLength();
  } else if (inner->IsSharedArrayBuffer()) {
    auto buf = inner.As<v8::SharedArrayBuffer>();
    return buf->ByteLength();
  } else {
    KJ_DASSERT(inner->IsArrayBufferView());
    auto view = inner.As<v8::ArrayBufferView>();
    auto buf = view->Buffer();
    if (buf->WasDetached()) [[unlikely]] {
      return 0;
    }
    return buf->ByteLength();
  }
}

// ======================================================================================
// JsUint8Array

kj::Maybe<JsUint8Array> JsUint8Array::tryCreate(Lock& js, size_t length) {
  JSG_REQUIRE(length < v8::ArrayBuffer::kMaxByteLength, RangeError, "The length is too large");
  auto backing = v8::ArrayBuffer::NewBackingStore(js.v8Isolate, length,
      v8::BackingStoreInitializationMode::kZeroInitialized,
      v8::BackingStoreOnFailureMode::kReturnNull);
  if (backing == nullptr) return kj::none;
  return create(js, kj::mv(backing), 0, length);
}

JsUint8Array JsUint8Array::create(Lock& js, size_t length) {
  JSG_REQUIRE(length < v8::ArrayBuffer::kMaxByteLength, RangeError, "The length is too large");
  auto backing = v8::ArrayBuffer::NewBackingStore(js.v8Isolate, length,
      v8::BackingStoreInitializationMode::kZeroInitialized,
      v8::BackingStoreOnFailureMode::kReturnNull);
  JSG_REQUIRE(backing != nullptr, RangeError, "Failed to allocate memory for Uint8Array");
  return create(js, kj::mv(backing), 0, length);
}

JsUint8Array JsUint8Array::create(Lock& js, kj::ArrayPtr<const kj::byte> data) {
  auto buf = create(js, data.size());
  buf.asArrayPtr().copyFrom(data);
  return buf;
}

JsUint8Array JsUint8Array::create(Lock& js, JsArrayBuffer& buffer) {
  v8::Local<v8::ArrayBuffer> ab = buffer;
  return JsUint8Array(v8::Uint8Array::New(ab, 0, ab->ByteLength()));
}

JsUint8Array JsUint8Array::create(Lock& js, JsSharedArrayBuffer& buffer) {
  v8::Local<v8::SharedArrayBuffer> ab = buffer;
  return JsUint8Array(v8::Uint8Array::New(ab, 0, ab->ByteLength()));
}

JsUint8Array JsUint8Array::create(
    Lock& js, std::unique_ptr<v8::BackingStore> backingStore, size_t byteOffset, size_t length) {
  return JsUint8Array(v8::Uint8Array::New(
      v8::ArrayBuffer::New(js.v8Isolate, kj::mv(backingStore)), byteOffset, length));
}

JsUint8Array JsUint8Array::slice(Lock& js, size_t newLength) const {
  JSG_REQUIRE(newLength <= size(), RangeError, "New length exceeds array length");
  return slice(js, 0, newLength);
}

kj::ArrayPtr<const kj::byte> JsUint8Array::asArrayPtr() const {
  auto buf = inner->Buffer();
  if (buf->WasDetached()) [[unlikely]] {
    return nullptr;
  }
  const kj::byte* data = static_cast<const kj::byte*>(buf->Data()) + inner->ByteOffset();
  size_t length = inner->ByteLength();
  return kj::ArrayPtr(data, length);
}

size_t JsUint8Array::size() const {
  return inner->ByteLength();
}

kj::Array<kj::byte> JsUint8Array::copy() {
  auto ptr = asArrayPtr();
  return kj::heapArray(ptr);
}

JsArrayBuffer JsUint8Array::getBuffer() const {
  auto buf = inner->Buffer();
  return JsArrayBuffer(buf);
}

bool JsUint8Array::isDetachable() const {
  auto buf = inner->Buffer();
  return buf->IsDetachable();
}

bool JsUint8Array::isDetached() const {
  auto buf = inner->Buffer();
  return buf->WasDetached();
}

void JsUint8Array::detachInPlace(Lock& js) {
  auto buf = inner->Buffer();
  check(buf->Detach({}));
}

JsUint8Array JsUint8Array::detachAndTake(Lock& js) {
  v8::Local<v8::Uint8Array> inner = *this;
  auto length = inner->ByteLength();
  auto offset = inner->ByteOffset();
  auto ab = getBuffer().detachAndTake(js);
  return JsUint8Array(v8::Uint8Array::New(ab, offset, length));
}

JsUint8Array JsUint8Array::slice(Lock& js, size_t offset, size_t length) const {
  auto buf = inner->Buffer();
  return JsUint8Array(v8::Uint8Array::New(buf, inner->ByteOffset() + offset, length));
}

bool JsUint8Array::isResizable() const {
  auto buf = inner->Buffer();
  return buf->IsResizableByUserJavaScript();
}

JsUint8Array::operator JsArrayBufferView() const {
  v8::Local<v8::Uint8Array> inner = *this;
  return jsg::JsArrayBufferView(inner);
}

JsUint8Array::operator JsBufferSource() const {
  v8::Local<v8::Uint8Array> inner = *this;
  return jsg::JsBufferSource(inner);
}

JsUint8Array JsUint8Array::clone(jsg::Lock& js) {
  auto buf = inner->Buffer();
  auto backing = buf->GetBackingStore();
  auto ab = jsg::JsArrayBuffer::create(js, kj::mv(backing));
  return JsUint8Array(v8::Uint8Array::New(ab, inner->ByteOffset(), inner->ByteLength()));
}

}  // namespace workerd::jsg
