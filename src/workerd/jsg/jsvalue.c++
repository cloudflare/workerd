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
