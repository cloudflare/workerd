#include "jsvalue.h"
#include "buffersource.h"
#include "ser.h"

namespace workerd::jsg {

JsValue::JsValue(v8::Local<v8::Value> inner) : inner(inner) {
  requireOnStack(this);
}

bool JsValue::operator==(const JsValue& other) const {
  return inner == other.inner;
}

JsObject::JsObject(v8::Local<v8::Map> inner) : JsObject(inner.As<v8::Object>()) {}

JsObject::operator JsMap() const {
  KJ_ASSERT(inner->IsMap());
  return JsMap(inner.As<v8::Map>());
}

void JsObject::set(Lock& js, const JsValue& name, const JsValue& value) {
  if (inner->IsMap()) {
    check(inner.As<v8::Map>()->Set(js.v8Context(), name.inner, value.inner));
  } else {
    check(inner->Set(js.v8Context(), name.inner, value.inner));
  }
}

void JsObject::set(Lock& js, kj::StringPtr name, const JsValue& value) {
  set(js, js.strIntern(name), value);
}

JsValue JsObject::get(Lock& js, const JsValue& name) {
  if (inner->IsMap()) {
    return JsValue(check(inner.As<v8::Map>()->Get(js.v8Context(), name.inner)));
  } else {
    return JsValue(check(inner->Get(js.v8Context(), name.inner)));
  }
}

JsValue JsObject::get(Lock& js, kj::StringPtr name) {
  return get(js, js.strIntern(name));
}

bool JsObject::has(Lock& js, const JsValue& name, HasOption option) {
  if (inner->IsMap()) {
    return check(inner.As<v8::Map>()->Has(js.v8Context(), name.inner));
  } else {
    if (option == HasOption::OWN) {
      KJ_ASSERT(name.inner->IsName());
      return check(inner->HasOwnProperty(js.v8Context(), name.inner.As<v8::Name>()));
    } else {
      return check(inner->Has(js.v8Context(), name.inner));
    }
  }
}

bool JsObject::has(Lock& js, kj::StringPtr name, HasOption option) {
  return has(js, js.strIntern(name), option);
}

void JsObject::delete_(Lock& js, const JsValue& name) {
  if (inner->IsMap()) {
    check(inner.As<v8::Map>()->Delete(js.v8Context(), name.inner));
  } else {
    check(inner->Delete(js.v8Context(), name.inner));
  }
}

void JsObject::delete_(Lock& js, kj::StringPtr name) {
  delete_(js, js.strIntern(name));
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
  return inner->GetIdentityHash();
}

kj::String JsObject::getConstructorName() {
  return kj::str(inner->GetConstructorName());
}

void JsObject::recursivelyFreeze(Lock& js) {
  jsg::recursivelyFreeze(js.v8Context(), inner);
}

JsObject JsObject::jsonClone(Lock& js) {
  auto tmp = JsValue(inner).toJson(js);
  auto obj = KJ_ASSERT_NONNULL(JsValue::fromJson(js, tmp).tryCast<jsg::JsObject>());
  return JsObject(obj);
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

#define V(Type) bool JsValue::is##Type () const { return inner->Is##Type(); }
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

uint32_t JsArray::size() const { return inner->Length(); }

JsValue JsArray::get(Lock& js, uint32_t i) const {
  return JsValue(check(inner->Get(js.v8Context(), i)));
}

JsArray::operator JsObject() const { return JsObject(inner.As<v8::Object>()); }

int JsString::length(jsg::Lock& js) const {
  return inner->Length();
}

int JsString::utf8Length(jsg::Lock& js) const {
  return inner->Utf8Length(js.v8Isolate);
}

kj::String JsString::toString(jsg::Lock& js) const {
  return kj::str(inner);
}

int JsString::hashCode() const {
  return inner->GetIdentityHash();
}

JsString JsString::concat(Lock& js, const JsString& one, const JsString& two) {
  return JsString(v8::String::Concat(js.v8Isolate, one.inner, two.inner));
}

bool JsString::operator==(const JsString& other) const {
  return inner->StringEquals(other.inner);
}

JsString::WriteIntoStatus JsString::writeInto(
    Lock& js,
    kj::ArrayPtr<char> buffer,
    WriteOptions options) const {
  WriteIntoStatus result = { 0, 0 };
  if (buffer.size() > 0) {
    result.written = inner->WriteUtf8(js.v8Isolate,
                                      buffer.begin(),
                                      buffer.size(),
                                      &result.read,
                                      options);
  }
  return result;
}

bool JsString::containsOnlyOneByte() const {
  return inner->ContainsOnlyOneByte();
}

kj::Maybe<JsArray> JsRegExp::operator()(Lock& js, const JsString& input) const {
  auto result = check(inner->Exec(js.v8Context(), input));
  if (result->IsNullOrUndefined()) return nullptr;
  return JsArray(result.As<v8::Array>());
}

kj::Maybe<JsArray> JsRegExp::operator()(Lock& js, kj::StringPtr input) const {
  auto result = check(inner->Exec(js.v8Context(), js.str(input)));
  if (result->IsNullOrUndefined()) return nullptr;
  return JsArray(result.As<v8::Array>());
}

jsg::ByteString JsDate::toUTCString(jsg::Lock& js) const {
  // TODO(cleanup): Add a toUTCString method to v8::Date to avoid having to grab
  // the implementation like this from the JS prototype.
  const auto stringify = js.v8Get(inner, "toUTCString"_kj);
  JSG_REQUIRE(stringify->IsFunction(), TypeError, "toUTCString on a Date is not a function");
  const auto stringified = jsg::check(stringify.template As<v8::Function>()->Call(
      js.v8Context(), inner, 0, nullptr));
  JSG_REQUIRE(stringified->IsString(), TypeError, "toUTCString on a Date did not return a string");
  JsString str(stringified.As<v8::String>());
  return jsg::ByteString(str.toString(js));
}

JsDate::operator kj::Date() const {
  return kj::UNIX_EPOCH + (int64_t(inner->ValueOf()) * kj::MILLISECONDS);
}

JsObject Lock::global() {
  return JsObject(v8Context()->Global());
}

JsValue Lock::undefined() {
  return JsValue(v8::Undefined(v8Isolate));
}

JsValue Lock::null() {
  return JsValue(v8::Null(v8Isolate));
}

JsBoolean Lock::boolean(bool val) {
  return JsBoolean(v8::Boolean::New(v8Isolate, val));
}

JsNumber Lock::num(double val) {
  return JsNumber(v8::Number::New(v8Isolate, val));
}

JsNumber Lock::num(float val) {
  return JsNumber(v8::Number::New(v8Isolate, val));
}

JsInt32 Lock::num(int8_t val) {
  return JsInt32(v8::Integer::New(v8Isolate, val).As<v8::Int32>());
}

JsInt32 Lock::num(int16_t val) {
  return JsInt32(v8::Integer::New(v8Isolate, val).As<v8::Int32>());
}

JsInt32 Lock::num(int32_t val) {
  return JsInt32(v8::Integer::New(v8Isolate, val).As<v8::Int32>());
}

JsBigInt Lock::bigInt(int64_t val) {
  return JsBigInt(v8::BigInt::New(v8Isolate, val));
}

JsUint32 Lock::num(uint8_t val) {
  return JsUint32(v8::Integer::NewFromUnsigned(v8Isolate, val).As<v8::Uint32>());
}

JsUint32 Lock::num(uint16_t val) {
  return JsUint32(v8::Integer::NewFromUnsigned(v8Isolate, val).As<v8::Uint32>());
}

JsUint32 Lock::num(uint32_t val) {
  return JsUint32(v8::Integer::NewFromUnsigned(v8Isolate, val).As<v8::Uint32>());
}

JsBigInt Lock::bigInt(uint64_t val) {
  return JsBigInt(v8::BigInt::NewFromUnsigned(v8Isolate, val));
}

JsString Lock::str() {
  return JsString(v8::String::Empty(v8Isolate));
}

JsString Lock::str(kj::ArrayPtr<const char16_t> str) {
  return JsString(check(v8::String::NewFromTwoByte(
      v8Isolate,
      reinterpret_cast<const uint16_t*>(str.begin()),
      v8::NewStringType::kNormal,
      str.size())));
}

JsString Lock::str(kj::ArrayPtr<const uint16_t> str) {
  return JsString(check(v8::String::NewFromTwoByte(
      v8Isolate,
      str.begin(),
      v8::NewStringType::kNormal,
      str.size())));
}

JsString Lock::str(kj::ArrayPtr<const char> str) {
  return JsString(check(v8::String::NewFromUtf8(
      v8Isolate,
      str.begin(),
      v8::NewStringType::kNormal,
      str.size())));
}

JsString Lock::str(kj::ArrayPtr<const kj::byte> str) {
  return JsString(check(v8::String::NewFromOneByte(
      v8Isolate,
      str.begin(),
      v8::NewStringType::kNormal,
      str.size())));
}

JsString Lock::strIntern(kj::StringPtr str) {
  return JsString(check(v8::String::NewFromUtf8(
      v8Isolate,
      str.begin(),
      v8::NewStringType::kInternalized,
      str.size())));
}

JsString Lock::strExtern(kj::ArrayPtr<const char> str) {
  return JsString(newExternalOneByteString(*this, str));
}

JsString Lock::strExtern(kj::ArrayPtr<const uint16_t> str) {
  return JsString(newExternalTwoByteString(*this, str));
}

JsRegExp Lock::regexp(kj::StringPtr str,
                      RegExpFlags flags,
                      kj::Maybe<uint32_t> backtrackLimit) {
  KJ_IF_MAYBE(limit, backtrackLimit) {
    return JsRegExp(check(v8::RegExp::NewWithBacktrackLimit(v8Context(), v8Str(v8Isolate, str),
        static_cast<v8::RegExp::Flags>(flags), *limit)));
  }
  return JsRegExp(check(v8::RegExp::New(v8Context(), v8Str(v8Isolate, str),
      static_cast<v8::RegExp::Flags>(flags))));
}

JsObject Lock::obj() {
  return JsObject(v8::Object::New(v8Isolate));
}

JsObject Lock::map() {
  return JsObject(v8::Map::New(v8Isolate));
}

JsValue Lock::external(void* ptr) {
  return JsValue(v8::External::New(v8Isolate, ptr));
}

JsValue Lock::error(kj::StringPtr message) {
  return JsValue(v8::Exception::Error(v8Str(v8Isolate, message)));
}

JsValue Lock::typeError(kj::StringPtr message) {
  return JsValue(v8::Exception::TypeError(v8Str(v8Isolate, message)));
}

JsValue Lock::rangeError(kj::StringPtr message) {
  return JsValue(v8::Exception::RangeError(v8Str(v8Isolate, message)));
}

BufferSource Lock::bytes(kj::Array<kj::byte> data) {
  return BufferSource(*this, BackingStore::from(kj::mv(data)));
}

JsSymbol Lock::symbol(kj::StringPtr str) {
  return JsSymbol(v8::Symbol::New(v8Isolate, v8StrIntern(v8Isolate, str)));
}

JsSymbol Lock::symbolShared(kj::StringPtr str) {
  return JsSymbol(v8::Symbol::For(v8Isolate, v8StrIntern(v8Isolate, str)));
}

JsSymbol Lock::symbolInternal(kj::StringPtr str) {
  return JsSymbol(v8::Symbol::ForApi(v8Isolate, v8StrIntern(v8Isolate, str)));
}

JsArray Lock::arr(kj::ArrayPtr<JsValue> values) {
  auto items = KJ_MAP(i, values) { return v8::Local<v8::Value>(i); };
  return JsArray(v8::Array::New(v8Isolate, items.begin(), items.size()));
}

#define V(Name) \
  JsSymbol Lock::symbol##Name() { \
    return JsSymbol(v8::Symbol::Get##Name(v8Isolate)); }
  JS_V8_SYMBOLS(V)
#undef V

JsDate Lock::date(double timestamp) {
  return JsDate(check(v8::Date::New(v8Context(), timestamp)).As<v8::Date>());
}

JsDate Lock::date(kj::Date date) {
  return JsDate(jsg::check(v8::Date::New(
      v8Context(), (date - kj::UNIX_EPOCH) / kj::MILLISECONDS)).As<v8::Date>());
}

JsDate Lock::date(kj::StringPtr date) {
  // TODO(cleanup): Add v8::Date::Parse API to avoid having to grab the constructor like this.
  const auto tmp = jsg::check(v8::Date::New(v8Context(), 0));
  KJ_REQUIRE(tmp->IsDate());
  const auto constructor = v8Get(tmp.As<v8::Object>(), "constructor"_kj);
  JSG_REQUIRE(constructor->IsFunction(), TypeError, "Date.constructor is not a function");
  v8::Local<v8::Value> argv = str(date);
  const auto converted = jsg::check(
      constructor.template As<v8::Function>()->NewInstance(v8Context(), 1, &argv));
  JSG_REQUIRE(converted->IsDate(), TypeError, "Date.constructor did not return a Date");
  return JsDate(converted.As<v8::Date>());
}

JsRef<JsValue> JsValue::addRef(Lock& js) {
  return JsRef<JsValue>(js, *this);
}

JsValue JsValue::structuredClone(Lock& js, kj::Maybe<kj::Array<JsValue>> maybeTransfers) {
  return jsg::structuredClone(js, *this, kj::mv(maybeTransfers));
}


}  // namespace workerd::jsg
