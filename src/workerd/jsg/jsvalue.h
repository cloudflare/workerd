#pragma once

#include "jsg.h"
#include <v8.h>

namespace workerd::jsg {

inline void requireOnStack(void* self) {
#ifdef KJ_DEBUG
  kj::requireOnStack(self, "JsValue types must be allocated on stack");
#endif
}

// The types listed in the JS_IS_TYPES macro are translated into is{Name}()
// methods on the JsValue type. These correspond directly to equivalent v8::Value
// types and therefore must be kept in sync.
#define JS_IS_TYPES(V) \
  V(Undefined) \
  V(Null) \
  V(NullOrUndefined) \
  V(True) \
  V(False) \
  V(ArgumentsObject) \
  V(NativeError) \
  V(Name) \
  V(Function) \
  V(AsyncFunction) \
  V(GeneratorFunction) \
  V(GeneratorObject) \
  V(WeakMap) \
  V(WeakSet) \
  V(WeakRef) \
  V(WasmNull) \
  V(ModuleNamespaceObject) \
  V(MapIterator) \
  V(SetIterator) \
  V(External) \
  V(BigIntObject) \
  V(BooleanObject) \
  V(NumberObject) \
  V(StringObject) \
  V(SymbolObject) \
  V(ArrayBuffer) \
  V(ArrayBufferView) \
  V(TypedArray) \
  V(Uint8Array) \
  V(Uint8ClampedArray) \
  V(Int8Array) \
  V(Uint16Array) \
  V(Int16Array) \
  V(Uint32Array) \
  V(Int32Array) \
  V(Float32Array) \
  V(Float64Array) \
  V(BigInt64Array) \
  V(BigUint64Array) \
  V(DataView) \
  V(SharedArrayBuffer) \
  V(WasmMemoryObject) \
  V(WasmModuleObject) \
  JS_TYPE_CLASSES(V)

template <typename TypeWrapper>
struct JsValueWrapper;

// Filters for `JsObject::getPropertyNames()`
enum PropertyFilter {
  ALL_PROPERTIES = 0,
  ONLY_WRITABLE = 1,
  ONLY_ENUMERABLE = 2,
  ONLY_CONFIGURABLE = 4,
  SKIP_STRINGS = 8,
  SKIP_SYMBOLS = 16
};
enum KeyCollectionFilter { OWN_ONLY, INCLUDE_PROTOTYPES };
enum IndexFilter { INCLUDE_INDICES, SKIP_INDICES };

enum PromiseState { PENDING, FULFILLED, REJECTED };

// A JsValue is an abstraction for a JavaScript value that has not been mapped
// to a C++ type. It wraps an underlying v8::Local<T> in order to avoid direct
// use of the v8 API in many cases. The JsValue (and JsRef<T>) are meant to
// fully replace (eventually) the use of jsg::V8Ref<T> and jsg::Value in
// addition to replacing direct use of v8::Local<T>.
//
// JsValue types (including the related JsBoolean, JsArray, JsObject, etc) can
// only be stack allocated and are not suitable for persistent storage of the
// value. To persist the JavaScript value, use JsRef<T>.
//
// The jsg::Lock instance is used to create instances of the Js* types. For
// example:
//
//   auto& js = Lock::from(isolate);
//   js.withinHandleScope([&] {
//     JsString str = js.str("foo");
//     JsNumber num = js.num(123);
//     JsArray arr = js.arr(js.str("foo"), js.num(123));
//     JsObject obj = js.obj();
//     obj.set(js, "foo", js.str("bar"));
//   });
//
// Note that the `js.withinHandleScope()` is only necessary if the code is not
// already running within a handle scope (which jsg mapped methods on jsg::Object
// instances always are).
//
// All of the Js* types can be trivially cast to JsValue via assignment.
//
//   JsValue val = js.str("foo");
//
// A JsValue can be trivially cast to a more specific type if the underlying
// JS type is compatible.
//
//   JsValue val = js.str("foo");
//   KJ_IF_SOME(str, val.tryCast<JsString>()) {
//     // str is a JsString
//   }
//   KJ_IF_SOME(num, val.tryCast<JsNumber>()) {
//     // never happens since val is not a number
//   }
//
// Because JsValue types are trivially assignable to v8::Local<v8::Value>
// they can be used together with TypeHandler<T> to convert to specific C++
// types:
//
//   auto obj = js.obj();
//   const TypeHandler<MyStruct>& handler = // ...
//   MyStruct v = KJ_ASSERT_NONNULL(handler.tryUnwrap(js, obj));
class JsValue final {
public:
  template <typename T>
  kj::Maybe<T> tryCast() const KJ_WARN_UNUSED_RESULT;

  operator v8::Local<v8::Value>() const { return inner; }

  bool operator==(const JsValue& other) const;

  bool isTruthy(Lock& js) const KJ_WARN_UNUSED_RESULT;
  kj::String toString(Lock& js) const KJ_WARN_UNUSED_RESULT;
  kj::String typeOf(Lock& js) const KJ_WARN_UNUSED_RESULT;
  JsString toJsString(Lock& js) const KJ_WARN_UNUSED_RESULT;

#define V(Type) bool is##Type() const KJ_WARN_UNUSED_RESULT;
  JS_IS_TYPES(V)
#undef V

  kj::String toJson(Lock& js) const KJ_WARN_UNUSED_RESULT;
  static JsValue fromJson(Lock& js, kj::ArrayPtr<const char> input) KJ_WARN_UNUSED_RESULT;
  static JsValue fromJson(Lock& js, const JsValue& input) KJ_WARN_UNUSED_RESULT;

  JsRef<JsValue> addRef(Lock& js) KJ_WARN_UNUSED_RESULT;

  JsValue structuredClone(Lock& js, kj::Maybe<kj::Array<JsValue>> maybeTransfers = kj::none)
      KJ_WARN_UNUSED_RESULT;

  template <typename T>
  static kj::Maybe<T&> tryGetExternal(Lock& js, const JsValue& value) KJ_WARN_UNUSED_RESULT;

  explicit JsValue(v8::Local<v8::Value> inner);

private:
  v8::Local<v8::Value> inner;
  friend class Lock;
  template <typename TypeWrapper> friend struct JsValueWrapper;
  template <typename T, typename Self> friend class JsBase;
  template <typename T> friend class JsRef;

#define V(Name) friend class Js##Name;
  JS_TYPE_CLASSES(V)
#undef V
};

template <typename T, typename Self>
class JsBase {
public:
  operator v8::Local<v8::Value>() const { return inner; }
  operator v8::Local<T>() const { return inner; }
  operator JsValue() const { return JsValue(inner.template As<v8::Value>()); }
  bool operator==(const JsValue& other) const KJ_WARN_UNUSED_RESULT {
    return inner == other.inner;
  }
  bool operator==(const JsBase& other) const KJ_WARN_UNUSED_RESULT {
    return inner == other.inner;
  }
  explicit JsBase(v8::Local<T> inner) : inner(inner) { requireOnStack(this); }
  JsRef<Self> addRef(Lock& js) KJ_WARN_UNUSED_RESULT;
private:
  v8::Local<T> inner;
  friend class Lock;
  friend class JsValue;
#define V(Name) friend class Js##Name;
  JS_TYPE_CLASSES(V)
#undef V
  template <typename TypeWrapper> friend struct JsValueWrapper;
  template <typename U> friend class JsRef;
};

class JsBoolean final : public JsBase<v8::Boolean, JsBoolean> {
public:
  bool value(Lock& js) const KJ_WARN_UNUSED_RESULT;

  using JsBase<v8::Boolean, JsBoolean>::JsBase;
};

class JsArray final : public JsBase<v8::Array, JsArray> {
public:
  operator JsObject() const;
  uint32_t size() const KJ_WARN_UNUSED_RESULT;
  JsValue get(Lock& js, uint32_t i) const KJ_WARN_UNUSED_RESULT;

  using JsBase<v8::Array, JsArray>::JsBase;
};

class JsString final : public JsBase<v8::String, JsString> {
public:
  int length(Lock& js) const KJ_WARN_UNUSED_RESULT;
  int utf8Length(Lock& js) const KJ_WARN_UNUSED_RESULT;
  kj::String toString(Lock& js) const KJ_WARN_UNUSED_RESULT;
  int hashCode() const;

  bool containsOnlyOneByte() const;

  bool operator==(const JsString& other) const;

  static JsString concat(Lock& js, const JsString& one, const JsString& two)
      KJ_WARN_UNUSED_RESULT;

  enum WriteOptions {
    NONE = v8::String::NO_OPTIONS,
    MANY_WRITES_EXPECTED = v8::String::HINT_MANY_WRITES_EXPECTED,
    NO_NULL_TERMINATION = v8::String::NO_NULL_TERMINATION,
    PRESERVE_ONE_BYTE_NULL = v8::String::PRESERVE_ONE_BYTE_NULL,
    REPLACE_INVALID_UTF8 = v8::String::REPLACE_INVALID_UTF8,
  };

  template <typename T>
  kj::Array<T> toArray(
      Lock& js,
      WriteOptions options = WriteOptions::NONE) const KJ_WARN_UNUSED_RESULT;

  struct WriteIntoStatus {
    // The number of elements (e.g. char, byte, uint16_t) read from this string.
    int read;
    // The number of elements (e.g. char, byte, uint16_t) written to the buffer.
    int written;
  };
  WriteIntoStatus writeInto(Lock& js,
                            kj::ArrayPtr<char> buffer,
                            WriteOptions options = WriteOptions::NONE) const;
  WriteIntoStatus writeInto(Lock& js,
                            kj::ArrayPtr<kj::byte> buffer,
                            WriteOptions options = WriteOptions::NONE) const;
  WriteIntoStatus writeInto(Lock& js,
                            kj::ArrayPtr<uint16_t> buffer,
                            WriteOptions options = WriteOptions::NONE) const;

  using JsBase<v8::String, JsString>::JsBase;
};

class JsRegExp final : public JsBase<v8::RegExp, JsRegExp> {
public:
  kj::Maybe<JsArray> operator()(Lock& js, const JsString& input) const KJ_WARN_UNUSED_RESULT;
  kj::Maybe<JsArray> operator()(Lock& js, kj::StringPtr input) const KJ_WARN_UNUSED_RESULT;
  using JsBase<v8::RegExp, JsRegExp>::JsBase;
};

class JsDate final : public JsBase<v8::Date, JsDate> {
public:
  jsg::ByteString toUTCString(Lock& js) const;
  operator kj::Date() const;
  using JsBase<v8::Date, JsDate>::JsBase;
};

// Note `jsg::JsPromise` and `jsg::Promise` are not the same things.
//
// `jsg::JsPromise` wraps an arbitrary `v8::Local<v8::Promise>` to avoid direct use of the V8 API.
// They have the same restrictions as other `JsValue`s (e.g. can only be stack allocated).
// `jsg::JsPromise` cannot be awaited in C++. They are opaque references to JavaScript promises.
//
// `jsg::Promise<T>` wraps an JavaScript promise to an instantiable C++ type `T` with syntax that
// makes it natural and ergonomic to consume within C++ (e.g. they provide a `then()` C++ method).
//
// You'll usually want to use `jsg::Promise<T>`. `jsg::JsPromise` should only be used when you need
// direct access to the promise state (e.g. the promise state or its fulfilled value).
class JsPromise final : public JsBase<v8::Promise, JsPromise> {
public:
  PromiseState state();
  JsValue result();
  using JsBase<v8::Promise, JsPromise>::JsBase;
};

class JsProxy final : public JsBase<v8::Proxy, JsProxy> {
public:
  JsValue target();
  JsValue handler();
  using JsBase<v8::Proxy, JsProxy>::JsBase;
};

#define V(Name) \
  class Js##Name final : public JsBase<v8::Name, Js##Name> { \
  public: \
    using JsBase<v8::Name, Js##Name>::JsBase; \
  };

  V(Symbol)
  V(BigInt)
  V(Number)
  V(Int32)
  V(Uint32)
  V(Set)

#undef V

class JsObject final : public JsBase<v8::Object, JsObject> {
public:
  template <typename T>
  bool isInstanceOf(Lock& js) {
    return js.getInstance(inner, typeid(T)) != kj::none;
  }

  template <typename T>
  kj::Maybe<jsg::Ref<T>> tryUnwrapAs(Lock& js) {
    KJ_IF_SOME(ins, js.getInstance(inner, typeid(T))) {
      return _jsgThis(static_cast<T*>(&ins));
    } else {
      return kj::none;
    }
  }

  void set(Lock& js, const JsValue& name, const JsValue& value);
  void set(Lock& js, kj::StringPtr name, const JsValue& value);
  JsValue get(Lock& js, const JsValue& name) KJ_WARN_UNUSED_RESULT;
  JsValue get(Lock& js, kj::StringPtr name) KJ_WARN_UNUSED_RESULT;

  enum class HasOption {
    NONE,
    OWN,
  };

  bool has(Lock& js, const JsValue& name, HasOption option = HasOption::NONE) KJ_WARN_UNUSED_RESULT;
  bool has(Lock& js, kj::StringPtr name, HasOption option = HasOption::NONE) KJ_WARN_UNUSED_RESULT;
  void delete_(Lock& js, const JsValue& name);
  void delete_(Lock& js, kj::StringPtr name);

  void setPrivate(Lock& js, kj::StringPtr name, const JsValue& value);
  JsValue getPrivate(Lock& js, kj::StringPtr name) KJ_WARN_UNUSED_RESULT;
  bool hasPrivate(Lock& js, kj::StringPtr name) KJ_WARN_UNUSED_RESULT;

  int hashCode() const;

  kj::String getConstructorName() KJ_WARN_UNUSED_RESULT;
  JsArray getPropertyNames(Lock& js, KeyCollectionFilter keyFilter, PropertyFilter propertyFilter,
                             IndexFilter indexFilter) KJ_WARN_UNUSED_RESULT;
  JsArray previewEntries(bool* isKeyValue) KJ_WARN_UNUSED_RESULT;

  // Returns the object's prototype, i.e. the property `__proto__`.
  //
  // Note that when called on a class constructor, this does NOT return `.prototype`, it still
  // returns `.__proto__`. Usefully, though, a class constructor's `__proto__` is always the
  // parent class's constructor.
  inline JsValue getPrototype() { return JsValue(inner->GetPrototype()); }

  using JsBase<v8::Object, JsObject>::JsBase;

  void recursivelyFreeze(Lock&);
  JsObject jsonClone(Lock&);
};

class JsMap final : public JsBase<v8::Map, JsMap> {
public:
  operator JsObject();

  void set(Lock& js, const JsValue& name, const JsValue& value);
  void set(Lock& js, kj::StringPtr name, const JsValue& value);
  JsValue get(Lock& js, const JsValue& name) KJ_WARN_UNUSED_RESULT;
  JsValue get(Lock& js, kj::StringPtr name) KJ_WARN_UNUSED_RESULT;
  bool has(Lock& js, const JsValue& name) KJ_WARN_UNUSED_RESULT;
  bool has(Lock& js, kj::StringPtr name) KJ_WARN_UNUSED_RESULT;
  void delete_(Lock& js, const JsValue& name);
  void delete_(Lock& js, kj::StringPtr name);

  int hashCode() const;

  using JsBase<v8::Map, JsMap>::JsBase;
};

template <typename T>
inline kj::Maybe<T> JsValue::tryCast() const {
  if constexpr (kj::isSameType<T, JsValue>()) { return JsValue(inner); }
#define V(Name) \
  else if constexpr (kj::isSameType<T, Js##Name>()) { \
    if (!inner->Is##Name()) return kj::none; \
    return T(inner.template As<v8::Name>()); \
  }
  JS_TYPE_CLASSES(V)
#undef V
  else { return kj::none; }
}

template <typename T>
inline kj::Maybe<T&> JsValue::tryGetExternal(Lock& js, const JsValue& value) {
  if (!value.isExternal()) return kj::none;
  return kj::Maybe<T&>(*static_cast<T*>(value.inner.As<v8::External>()->Value()));
}

template <typename T>
inline kj::Array<T> JsString::toArray(Lock& js, WriteOptions options) const {
  if constexpr (kj::isSameType<T, kj::byte>()) {
    KJ_ASSERT(inner->ContainsOnlyOneByte());
    auto buf = kj::heapArray<kj::byte>(inner->Length());
    inner->WriteOneByte(js.v8Isolate, buf.begin(), 0, buf.size(), options);
    return kj::mv(buf);
  } else {
    auto buf = kj::heapArray<uint16_t>(inner->Length());
    inner->Write(js.v8Isolate, buf.begin(), 0, buf.size(), options);
    return kj::mv(buf);
  }
}

template <typename...Args> requires (std::assignable_from<JsValue&, Args> && ...)
inline JsArray Lock::arr(const Args&... args) {
  v8::Local<v8::Value> values[] = { args... };
  return JsArray(v8::Array::New(v8Isolate, &values[0], sizeof...(Args)));
}

template <typename...Args> requires (std::assignable_from<JsValue&, Args> && ...)
inline JsSet Lock::set(const Args&...args) {
  auto set = v8::Set::New(v8Isolate);
  (check(set->Add(v8Context(), args.inner)), ...);
  return JsSet(set);
}

// A persistent handle for a Js* type suitable for storage and gc visitable.
//
// For example,
//
//   class Foo : public jsg::Object {
//   public:
//     void setStored(jsg::Lock& js, jsg::JsValue value) {
//       stored = value.addRef(js);
//     }
//     JsValue getStored(jsg::Lock& js) {
//       return stored.getHandle(js);
//     }
//     JSG_RESOURCE_TYPE(Foo) {
//       JSG_PROTOTYPE_PROPERTY(stored, getStored, setStored);
//     }
//   private:
//     jsg::JsRef<JsValue> stored;
//
//     void visitForGc(GcVisitor& visitor) { visitor.visit(stored); }
//   };
template <typename T>
class JsRef final {
  static_assert(std::is_assignable_v<JsValue, T>, "JsRef<T>, T must be assignable to type JsValue");
public:
  JsRef(): JsRef(nullptr) {}
  JsRef(decltype(nullptr)): value(nullptr) {}
  JsRef(Lock& js, const T& value) : value(js.v8Isolate, value.inner) {}
  JsRef(JsRef<T>& other) = delete;
  JsRef(JsRef<T>&& other) = default;
  template <typename U>
  JsRef(Lock& js, V8Ref<U>&& v8Value)
      : value(js.v8Isolate, v8Value.getHandle(js).template As<v8::Value>()) {}
  JsRef& operator=(JsRef<T>& other) = delete;
  JsRef& operator=(JsRef<T>&& other) = default;

  T getHandle(Lock& js) KJ_WARN_UNUSED_RESULT {
    JsValue handle(value.getHandle(js));
    return KJ_ASSERT_NONNULL(handle.tryCast<T>());
  }

  JsRef<T> addRef(Lock& js) KJ_WARN_UNUSED_RESULT {
    return JsRef<T>(js, getHandle(js));
  }

  bool operator==(const JsRef<T>& other) {
    return value == other.value;
  }

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(value);
  }

  // Supported only to allow for an easier transition for code that still
  // requires V8Ref types.
  template <typename U>
  V8Ref<U> addV8Ref(Lock& js) KJ_WARN_UNUSED_RESULT { return value.addRef(js); }

  // Supported only to allow for an easier transition for code that still
  // requires V8Ref types.
  template <typename U>
  operator V8Ref<U>() && { return kj::mv(value).template cast<U>(
      Lock::from(v8::Isolate::GetCurrent())); }

  JSG_MEMORY_INFO(JsRef) {
    tracker.trackField("value", value);
  }

private:
  Value value;
  friend class JsValue;
#define V(Name) friend class Js##Name;
  JS_TYPE_CLASSES(V)
#undef V

  friend class MemoryTracker;
};

template <typename T, typename Self>
inline JsRef<Self> JsBase<T,Self>::addRef(Lock& js) {
  return JsRef<Self>(js, *static_cast<Self*>(this));
}

inline kj::String KJ_STRINGIFY(const JsValue& value) {
  return value.toString(Lock::from(v8::Isolate::GetCurrent()));
}

template <typename TypeWrapper>
struct JsValueWrapper {
#define TYPES_TO_WRAP(V) \
  V(Value) \
  JS_TYPE_CLASSES(V)

  template <typename T, typename = kj::EnableIf<std::is_assignable_v<JsValue, T>>>
  static constexpr const std::type_info& getName(T*) { return typeid(T); }

  template <typename T, typename = kj::EnableIf<std::is_assignable_v<JsValue, T>>>
  static constexpr const std::type_info& getName(JsRef<T>*) { return typeid(T); }

#define V(Name) \
  v8::Local<v8::Name> wrap(v8::Local<v8::Context> context, \
                           kj::Maybe<v8::Local<v8::Object>> creator, \
                           Js##Name value) { \
    return value; \
  } \
  v8::Local<v8::Name> wrap(v8::Local<v8::Context> context, \
                           kj::Maybe<v8::Local<v8::Object>> creator, \
                           JsRef<Js##Name> value) { \
    return value.getHandle(Lock::from(context->GetIsolate())); \
  }

  TYPES_TO_WRAP(V)
#undef V

  template <typename T, typename = kj::EnableIf<std::is_assignable_v<JsValue, T>>>
  kj::Maybe<T> tryUnwrap(v8::Local<v8::Context> context,
                         v8::Local<v8::Value> handle,
                         T*, kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if constexpr (kj::isSameType<T, JsString>()) {
      return T(check(handle->ToString(context)));
    } else if constexpr (kj::isSameType<T, JsBoolean>()) {
      return T(handle->ToBoolean(context->GetIsolate()));
    } else {
      JsValue value(handle);
      KJ_IF_SOME(t, value.tryCast<T>()) {
        return t;
      }
      return kj::none;
    }
  }

  template <typename T, typename = kj::EnableIf<std::is_assignable_v<JsValue, T>>>
  kj::Maybe<JsRef<T>> tryUnwrap(v8::Local<v8::Context> context,
                                v8::Local<v8::Value> handle,
                                JsRef<T>*,
                                kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto isolate = context->GetIsolate();
    auto& js = Lock::from(isolate);
    KJ_IF_SOME(result, TypeWrapper::from(isolate)
        .tryUnwrap(context, handle, (T*)nullptr, parentObject)) {
      return JsRef(js, result);
    }
    return kj::none;
  }
};

class JsMessage final {
public:
  static JsMessage create(Lock& js, const JsValue& exception);
  explicit inline JsMessage() : inner(v8::Local<v8::Message>()) {
    requireOnStack(this);
  }
  explicit inline JsMessage(v8::Local<v8::Message> inner) : inner(inner) {
    requireOnStack(this);
  }
  operator v8::Local<v8::Message>() const { return inner; }

  // Is it possible for the underlying v8::Local<v8::Message> to be
  // empty, in which case the bool() operator will return false.
  operator bool() const { return !inner.IsEmpty(); }

  // Adds the JS Stack associated with this JsMessage to the given
  // kj::Vector.
  void addJsStackTrace(Lock& js, kj::Vector<kj::String>& lines);

private:
  v8::Local<v8::Message> inner;
};

}  // namespace workerd::jsg
