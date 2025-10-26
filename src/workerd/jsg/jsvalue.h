#pragma once

#include "jsg.h"

#include <v8-container.h>
#include <v8-date.h>
#include <v8-external.h>

namespace workerd::jsg {

inline void requireOnStack(void* self) {
#ifdef KJ_DEBUG
  kj::requireOnStack(self, "JsValue types must be allocated on stack");
#endif
}

// The types listed in the JS_IS_TYPES macro are translated into is{Name}()
// methods on the JsValue type. These correspond directly to equivalent v8::Value
// types and therefore must be kept in sync.
#define JS_IS_TYPES(V)                                                                             \
  V(Undefined)                                                                                     \
  V(Null)                                                                                          \
  V(NullOrUndefined)                                                                               \
  V(True)                                                                                          \
  V(False)                                                                                         \
  V(ArgumentsObject)                                                                               \
  V(NativeError)                                                                                   \
  V(Name)                                                                                          \
  V(AsyncFunction)                                                                                 \
  V(GeneratorFunction)                                                                             \
  V(GeneratorObject)                                                                               \
  V(WeakMap)                                                                                       \
  V(WeakSet)                                                                                       \
  V(WeakRef)                                                                                       \
  V(WasmNull)                                                                                      \
  V(ModuleNamespaceObject)                                                                         \
  V(MapIterator)                                                                                   \
  V(SetIterator)                                                                                   \
  V(External)                                                                                      \
  V(BigIntObject)                                                                                  \
  V(BooleanObject)                                                                                 \
  V(NumberObject)                                                                                  \
  V(StringObject)                                                                                  \
  V(SymbolObject)                                                                                  \
  V(ArrayBuffer)                                                                                   \
  V(ArrayBufferView)                                                                               \
  V(TypedArray)                                                                                    \
  V(Uint8ClampedArray)                                                                             \
  V(Int8Array)                                                                                     \
  V(Uint16Array)                                                                                   \
  V(Int16Array)                                                                                    \
  V(Uint32Array)                                                                                   \
  V(Int32Array)                                                                                    \
  V(Float16Array)                                                                                  \
  V(Float32Array)                                                                                  \
  V(Float64Array)                                                                                  \
  V(BigInt64Array)                                                                                 \
  V(BigUint64Array)                                                                                \
  V(DataView)                                                                                      \
  V(SharedArrayBuffer)                                                                             \
  V(WasmMemoryObject)                                                                              \
  V(WasmModuleObject)                                                                              \
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

  operator v8::Local<v8::Value>() const {
    return inner;
  }

  bool operator==(const JsValue& other) const;
  bool strictEquals(const JsValue& other) const;

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

  JsValue structuredClone(
      Lock& js, kj::Maybe<kj::Array<JsValue>> maybeTransfers = kj::none) KJ_WARN_UNUSED_RESULT;

  template <typename T>
  static kj::Maybe<T&> tryGetExternal(Lock& js, const JsValue& value) KJ_WARN_UNUSED_RESULT;

  explicit JsValue(v8::Local<v8::Value> inner);

 private:
  v8::Local<v8::Value> inner;
  friend class Lock;
  template <typename TypeWrapper>
  friend struct JsValueWrapper;
  template <typename T, typename Self>
  friend class JsBase;
  template <typename T>
  friend class JsRef;

#define V(Name) friend class Js##Name;
  JS_TYPE_CLASSES(V)
#undef V
};

template <typename T, typename Self>
class JsBase {
 public:
  operator v8::Local<v8::Value>() const {
    return inner;
  }
  operator v8::Local<T>() const {
    return inner;
  }
  operator JsValue() const {
    return JsValue(inner.template As<v8::Value>());
  }
  bool operator==(const JsValue& other) const KJ_WARN_UNUSED_RESULT {
    return inner == other.inner;
  }
  bool operator==(const JsBase& other) const KJ_WARN_UNUSED_RESULT {
    return inner == other.inner;
  }
  explicit JsBase(v8::Local<T> inner): inner(inner) {
    requireOnStack(this);
  }
  JsRef<Self> addRef(Lock& js) KJ_WARN_UNUSED_RESULT;

 private:
  v8::Local<T> inner;
  friend class Lock;
  friend class JsValue;
#define V(Name) friend class Js##Name;
  JS_TYPE_CLASSES(V)
#undef V
  template <typename TypeWrapper>
  friend struct JsValueWrapper;
  template <typename U>
  friend class JsRef;
};

class JsBoolean final: public JsBase<v8::Boolean, JsBoolean> {
 public:
  bool value(Lock& js) const KJ_WARN_UNUSED_RESULT;

  using JsBase<v8::Boolean, JsBoolean>::JsBase;
};

class JsArray final: public JsBase<v8::Array, JsArray> {
 public:
  operator JsObject() const;
  uint32_t size() const KJ_WARN_UNUSED_RESULT;
  JsValue get(Lock& js, uint32_t i) const KJ_WARN_UNUSED_RESULT;
  void add(Lock& js, const JsValue& value);

  using JsBase<v8::Array, JsArray>::JsBase;
};

class JsUint8Array final: public JsBase<v8::Uint8Array, JsUint8Array> {
 public:
  template <typename T = kj::byte>
  kj::ArrayPtr<T> asArrayPtr() {
    v8::Local<v8::Uint8Array> inner = *this;
    auto buf = inner->Buffer();
    T* data = static_cast<T*>(buf->Data()) + inner->ByteOffset();
    size_t length = inner->ByteLength();
    return kj::ArrayPtr(data, length);
  }

  using JsBase<v8::Uint8Array, JsUint8Array>::JsBase;
};

class JsString final: public JsBase<v8::String, JsString> {
 public:
  int length(Lock& js) const KJ_WARN_UNUSED_RESULT;
  size_t utf8Length(Lock& js) const KJ_WARN_UNUSED_RESULT;
  kj::String toString(Lock& js) const KJ_WARN_UNUSED_RESULT;
  jsg::USVString toUSVString(Lock& js) const KJ_WARN_UNUSED_RESULT;
  jsg::ByteString toByteString(Lock& js) const KJ_WARN_UNUSED_RESULT;
  jsg::DOMString toDOMString(Lock& js) const KJ_WARN_UNUSED_RESULT;

  int hashCode() const;

  bool containsOnlyOneByte() const;

  bool operator==(const JsString& other) const;

  // "Internalize" the string. Returns a string with the same content but which is identity-equal
  // to all other internalized strings with the same content. If the string is already
  // internalized, this returns the same value. Note that strings originating from literals in the
  // code are always internalized.
  JsString internalize(Lock& js) const;

  static JsString concat(Lock& js, const JsString& one, const JsString& two) KJ_WARN_UNUSED_RESULT;

  enum WriteFlags {
    NONE = v8::String::WriteFlags::kNone,
    NULL_TERMINATION = v8::String::WriteFlags::kNullTerminate,
    REPLACE_INVALID_UTF8 = v8::String::WriteFlags::kReplaceInvalidUtf8,
  };

  template <typename T>
  kj::Array<T> toArray(Lock& js, WriteFlags options = WriteFlags::NONE) const KJ_WARN_UNUSED_RESULT;

  struct WriteIntoStatus {
    // The number of elements (e.g. char, byte, uint16_t) read from this string.
    size_t read;
    // The number of elements (e.g. char, byte, uint16_t) written to the buffer.
    size_t written;
  };
  WriteIntoStatus writeInto(
      Lock& js, kj::ArrayPtr<char> buffer, WriteFlags options = WriteFlags::NONE) const;
  WriteIntoStatus writeInto(
      Lock& js, kj::ArrayPtr<kj::byte> buffer, WriteFlags options = WriteFlags::NONE) const;
  WriteIntoStatus writeInto(
      Lock& js, kj::ArrayPtr<uint16_t> buffer, WriteFlags options = WriteFlags::NONE) const;

  using JsBase<v8::String, JsString>::JsBase;
};

class JsRegExp final: public JsBase<v8::RegExp, JsRegExp> {
 public:
  kj::Maybe<JsArray> operator()(Lock& js, const JsString& input) const KJ_WARN_UNUSED_RESULT;
  kj::Maybe<JsArray> operator()(Lock& js, kj::StringPtr input) const KJ_WARN_UNUSED_RESULT;
  using JsBase<v8::RegExp, JsRegExp>::JsBase;

  bool match(Lock& js, kj::StringPtr input);
};

class JsDate final: public JsBase<v8::Date, JsDate> {
 public:
  jsg::ByteString toUTCString(Lock& js) const;
  jsg::ByteString toISOString(Lock& js) const;
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
class JsPromise final: public JsBase<v8::Promise, JsPromise> {
 public:
  PromiseState state();
  JsValue result();
  using JsBase<v8::Promise, JsPromise>::JsBase;
};

class JsProxy final: public JsBase<v8::Proxy, JsProxy> {
 public:
  JsValue target();
  JsValue handler();
  using JsBase<v8::Proxy, JsProxy>::JsBase;
};

#define V(Name)                                                                                    \
  class Js##Name final: public JsBase<v8::Name, Js##Name> {                                        \
   public:                                                                                         \
    using JsBase<v8::Name, Js##Name>::JsBase;                                                      \
  };

V(Symbol)
V(BigInt)
V(Int32)
V(Uint32)
V(Set)

#undef V

class JsNumber final: public JsBase<v8::Number, JsNumber> {
 public:
  kj::Maybe<double> value(Lock& js) const KJ_WARN_UNUSED_RESULT;
  bool isSafeInteger(Lock& js) const KJ_WARN_UNUSED_RESULT;
  kj::Maybe<double> toSafeInteger(Lock& js) const KJ_WARN_UNUSED_RESULT;

  using JsBase<v8::Number, JsNumber>::JsBase;
};

class JsObject final: public JsBase<v8::Object, JsObject> {
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
  void setReadOnly(Lock& js, kj::StringPtr name, const JsValue& value);
  void setNonEnumerable(Lock& js, const JsSymbol& name, const JsValue& value);

  // Like set but uses the defineProperty API instead in order to override
  // the default property attributes. This is useful for defining properties
  // that otherwise would not be normally settable, such as the name of an
  // error object.
  void defineProperty(Lock& js, kj::StringPtr name, const JsValue& value);

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
  JsArray getPropertyNames(Lock& js,
      KeyCollectionFilter keyFilter,
      PropertyFilter propertyFilter,
      IndexFilter indexFilter) KJ_WARN_UNUSED_RESULT;
  JsArray previewEntries(bool* isKeyValue) KJ_WARN_UNUSED_RESULT;

  // Returns the object's prototype, i.e. the property `__proto__`.
  //
  // Note that when called on a class constructor, this does NOT return `.prototype`, it still
  // returns `.__proto__`. Usefully, though, a class constructor's `__proto__` is always the
  // parent class's constructor.
  JsValue getPrototype(Lock& js) KJ_WARN_UNUSED_RESULT;

  using JsBase<v8::Object, JsObject>::JsBase;

  void recursivelyFreeze(Lock&);
  void seal(Lock&);
  JsObject jsonClone(Lock&);
};

// Defined here because `JsObject` is an incomplete type in `jsg.h`.
template <typename T>
inline JsObject Lock::getPrototypeFor() {
  return JsObject(getPrototypeFor(typeid(T)));
}

class JsMap final: public JsBase<v8::Map, JsMap> {
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
  if constexpr (kj::isSameType<T, JsValue>()) {
    return JsValue(inner);
  }
#define V(Name)                                                                                    \
  else if constexpr (kj::isSameType<T, Js##Name>()) {                                              \
    if (!inner->Is##Name()) return kj::none;                                                       \
    return T(inner.template As<v8::Name>());                                                       \
  }
  JS_TYPE_CLASSES(V)
#undef V
  else {
    return kj::none;
  }
}

template <typename T>
inline kj::Maybe<T&> JsValue::tryGetExternal(Lock& js, const JsValue& value) {
  if (!value.isExternal()) return kj::none;
  return kj::Maybe<T&>(*static_cast<T*>(value.inner.As<v8::External>()->Value()));
}

template <typename T>
inline kj::Array<T> JsString::toArray(Lock& js, WriteFlags options) const {
  if constexpr (kj::isSameType<T, kj::byte>()) {
    KJ_DASSERT(inner->ContainsOnlyOneByte());
    auto buf = kj::heapArray<kj::byte>(inner->Length());
    inner->WriteOneByteV2(js.v8Isolate, 0, buf.size(), buf.begin(), options);
    return kj::mv(buf);
  } else {
    auto buf = kj::heapArray<uint16_t>(inner->Length());
    inner->WriteV2(js.v8Isolate, 0, buf.size(), buf.begin(), options);
    return kj::mv(buf);
  }
}

template <typename... Args>
  requires(std::assignable_from<JsValue&, Args> && ...)
inline JsArray Lock::arr(const Args&... args) {
  v8::Local<v8::Value> values[] = {args...};
  return JsArray(v8::Array::New(v8Isolate, &values[0], sizeof...(Args)));
}

template <typename T, typename Func>
inline JsArray Lock::arr(kj::ArrayPtr<T> values, Func fn) {
  v8::LocalVector<v8::Value> vec(v8Isolate);
  vec.reserve(values.size());
  for (const T& val: values) {
    vec.push_back(fn(*this, val));
  }
  return JsArray(v8::Array::New(v8Isolate, vec.data(), vec.size()));
}

template <typename... Args>
  requires(std::assignable_from<JsValue&, Args> && ...)
inline JsSet Lock::set(const Args&... args) {
  auto set = v8::Set::New(v8Isolate);
  (check(set->Add(v8Context(), args.inner)), ...);
  return JsSet(set);
}

template <typename T>
inline JsObject Lock::opaque(T&& inner) {
  auto wrapped = wrapOpaque(v8Context(), kj::mv(inner));
  KJ_ASSERT(!wrapped.IsEmpty());
  KJ_ASSERT(wrapped->IsObject());
  return JsObject(wrapped.template As<v8::Object>());
}

class JsFunction final: public JsBase<v8::Function, JsFunction> {
 public:
  using JsBase<v8::Function, JsFunction>::JsBase;

  // Calls the function with the given receiver and arguments.
  template <IsJsValue... Args>
  JsValue call(Lock& js, const JsValue& recv, Args... args) const {
    v8::Local<v8::Function> fn = *this;
    v8::Local<v8::Value> argv[] = {args...};
    return JsValue(check(fn->Call(js.v8Context(), recv, sizeof...(Args), argv)));
  }

  // Calls the function with a null receiver and arguments.
  template <IsJsValue... Args>
  JsValue callNoReceiver(Lock& js, Args... args) const {
    return call(js, js.null(), kj::fwd<Args...>(args...));
  }

  // Calls the function with the given receiver and arguments.
  JsValue call(Lock& js, const JsValue& recv, v8::LocalVector<v8::Value>& args) const;

  // Calls the function with a null receiver and arguments. When null is passed
  // as the receiver, the global object is used instead.
  JsValue callNoReceiver(Lock& js, v8::LocalVector<v8::Value>& args) const;

  // Gets the function's length property.
  size_t length(Lock& js) const;

  // Gets the function's name property.
  JsString name(Lock& js) const;

  // Not guaranteed to be unique, but will be the same for the same function.
  // Use the JsValue strictEquals() method for true identity comparison.
  uint hashCode() const;

  operator JsObject() const {
    return JsObject(inner);
  }
};

// A persistent handle for a Js* type suitable for storage and GC visitable.
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
  JsRef(Lock& js, const T& value): value(js.v8Isolate, value.inner) {}
  JsRef(JsRef<T>& other) = delete;
  JsRef(JsRef<T>&& other) = default;
  template <typename U>
  JsRef(Lock& js, V8Ref<U>&& v8Value)
      : value(js.v8Isolate, v8Value.getHandle(js).template As<v8::Value>()) {}
  JsRef& operator=(JsRef<T>& other) = delete;
  JsRef& operator=(JsRef<T>&& other) = default;

  T getHandle(Lock& js) const KJ_WARN_UNUSED_RESULT {
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
  V8Ref<U> addV8Ref(Lock& js) KJ_WARN_UNUSED_RESULT {
    return value.addRef(js);
  }

  // Supported only to allow for an easier transition for code that still
  // requires V8Ref types.
  template <typename U>
  operator V8Ref<U>() && {
    return kj::mv(value).template cast<U>(jsg::Lock::current());
  }

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
inline JsRef<Self> JsBase<T, Self>::addRef(Lock& js) {
  return JsRef<Self>(js, *static_cast<Self*>(this));
}

inline kj::String KJ_STRINGIFY(const JsValue& value) {
  return value.toString(jsg::Lock::current());
}

template <typename TypeWrapper>
struct JsValueWrapper {
#define TYPES_TO_WRAP(V)                                                                           \
  V(Value)                                                                                         \
  JS_TYPE_CLASSES(V)

  template <typename T, typename = kj::EnableIf<std::is_assignable_v<JsValue, T>>>
  static constexpr const std::type_info& getName(T*) {
    return typeid(T);
  }

  template <typename T, typename = kj::EnableIf<std::is_assignable_v<JsValue, T>>>
  static constexpr const std::type_info& getName(JsRef<T>*) {
    return typeid(T);
  }

#define V(Name)                                                                                    \
  v8::Local<v8::Name> wrap(jsg::Lock& js, v8::Local<v8::Context> context,                          \
      kj::Maybe<v8::Local<v8::Object>> creator, Js##Name value) {                                  \
    return value;                                                                                  \
  }                                                                                                \
  v8::Local<v8::Name> wrap(jsg::Lock& js, v8::Local<v8::Context> context,                          \
      kj::Maybe<v8::Local<v8::Object>> creator, JsRef<Js##Name> value) {                           \
    return value.getHandle(js);                                                                    \
  }

  TYPES_TO_WRAP(V)
#undef V

  template <typename T, typename = kj::EnableIf<std::is_assignable_v<JsValue, T>>>
  kj::Maybe<T> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      T*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if constexpr (kj::isSameType<T, JsString>()) {
      return T(check(handle->ToString(context)));
    } else if constexpr (kj::isSameType<T, JsBoolean>()) {
      return T(handle->ToBoolean(js.v8Isolate));
    } else if constexpr (kj::isSameType<T, JsNumber>()) {
      return T(check(handle->ToNumber(context)));
    } else {
      JsValue value(handle);
      KJ_IF_SOME(t, value.tryCast<T>()) {
        return t;
      }
      return kj::none;
    }
  }

  template <typename T, typename = kj::EnableIf<std::is_assignable_v<JsValue, T>>>
  kj::Maybe<JsRef<T>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      JsRef<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto isolate = js.v8Isolate;
    KJ_IF_SOME(result,
        TypeWrapper::from(isolate).tryUnwrap(js, context, handle, (T*)nullptr, parentObject)) {
      return JsRef(js, result);
    }
    return kj::none;
  }
};

class JsMessage final {
 public:
  static JsMessage create(Lock& js, const JsValue& exception);
  explicit inline JsMessage(): inner(v8::Local<v8::Message>()) {
    requireOnStack(this);
  }
  explicit inline JsMessage(v8::Local<v8::Message> inner): inner(inner) {
    requireOnStack(this);
  }
  operator v8::Local<v8::Message>() const {
    return inner;
  }

  // Is it possible for the underlying v8::Local<v8::Message> to be
  // empty, in which case the bool() operator will return false.
  operator bool() const {
    return !inner.IsEmpty();
  }

  // Adds the JS Stack associated with this JsMessage to the given
  // kj::Vector.
  void addJsStackTrace(Lock& js, kj::Vector<kj::String>& lines);

 private:
  v8::Local<v8::Message> inner;
};

inline JsObject Lock::global() {
  return JsObject(v8Context()->Global());
}

inline JsValue Lock::undefined() {
  return JsValue(v8::Undefined(v8Isolate));
}

inline JsValue Lock::null() {
  return JsValue(v8::Null(v8Isolate));
}

inline JsBoolean Lock::boolean(bool val) {
  return JsBoolean(v8::Boolean::New(v8Isolate, val));
}

inline JsNumber Lock::num(double val) {
  return JsNumber(v8::Number::New(v8Isolate, val));
}

inline JsNumber Lock::num(float val) {
  return JsNumber(v8::Number::New(v8Isolate, val));
}

inline JsInt32 Lock::num(int8_t val) {
  return JsInt32(v8::Integer::New(v8Isolate, val).As<v8::Int32>());
}

inline JsInt32 Lock::num(int16_t val) {
  return JsInt32(v8::Integer::New(v8Isolate, val).As<v8::Int32>());
}

inline JsInt32 Lock::num(int32_t val) {
  return JsInt32(v8::Integer::New(v8Isolate, val).As<v8::Int32>());
}

inline JsBigInt Lock::bigInt(int64_t val) {
  return JsBigInt(v8::BigInt::New(v8Isolate, val));
}

inline JsUint32 Lock::num(uint8_t val) {
  return JsUint32(v8::Integer::NewFromUnsigned(v8Isolate, val).As<v8::Uint32>());
}

inline JsUint32 Lock::num(uint16_t val) {
  return JsUint32(v8::Integer::NewFromUnsigned(v8Isolate, val).As<v8::Uint32>());
}

inline JsUint32 Lock::num(uint32_t val) {
  return JsUint32(v8::Integer::NewFromUnsigned(v8Isolate, val).As<v8::Uint32>());
}

inline JsBigInt Lock::bigInt(uint64_t val) {
  return JsBigInt(v8::BigInt::NewFromUnsigned(v8Isolate, val));
}

inline JsString Lock::str() {
  return JsString(v8::String::Empty(v8Isolate));
}

inline JsString Lock::str(kj::ArrayPtr<const char16_t> str) {
  return JsString(check(v8::String::NewFromTwoByte(v8Isolate,
      reinterpret_cast<const uint16_t*>(str.begin()), v8::NewStringType::kNormal, str.size())));
}

inline JsString Lock::str(kj::ArrayPtr<const uint16_t> str) {
  return JsString(check(
      v8::String::NewFromTwoByte(v8Isolate, str.begin(), v8::NewStringType::kNormal, str.size())));
}

inline JsString Lock::str(kj::ArrayPtr<const char> str) {
  return JsString(check(
      v8::String::NewFromUtf8(v8Isolate, str.begin(), v8::NewStringType::kNormal, str.size())));
}

inline JsString Lock::str(kj::ArrayPtr<const kj::byte> str) {
  return JsString(check(
      v8::String::NewFromOneByte(v8Isolate, str.begin(), v8::NewStringType::kNormal, str.size())));
}

inline JsString Lock::strIntern(kj::StringPtr str) {
  return JsString(check(v8::String::NewFromUtf8(
      v8Isolate, str.begin(), v8::NewStringType::kInternalized, str.size())));
}

inline JsString Lock::strExtern(kj::ArrayPtr<const char> str) {
  return JsString(newExternalOneByteString(*this, str));
}

inline JsString Lock::strExtern(kj::ArrayPtr<const uint16_t> str) {
  return JsString(newExternalTwoByteString(*this, str));
}

inline JsObject Lock::obj() {
  return JsObject(v8::Object::New(v8Isolate));
}

inline JsObject Lock::objNoProto() {
  return JsObject(v8::Object::New(v8Isolate, v8::Null(v8Isolate), nullptr, nullptr, 0));
}

inline JsMap Lock::map() {
  return JsMap(v8::Map::New(v8Isolate));
}

inline JsValue Lock::external(void* ptr) {
  return JsValue(v8::External::New(v8Isolate, ptr));
}

inline JsValue Lock::error(kj::StringPtr message) {
  return JsValue(v8::Exception::Error(v8Str(v8Isolate, message)));
}

inline JsValue Lock::typeError(kj::StringPtr message) {
  return JsValue(v8::Exception::TypeError(v8Str(v8Isolate, message)));
}

inline JsValue Lock::rangeError(kj::StringPtr message) {
  return JsValue(v8::Exception::RangeError(v8Str(v8Isolate, message)));
}

inline JsSymbol Lock::symbol(kj::StringPtr str) {
  return JsSymbol(v8::Symbol::New(v8Isolate, v8StrIntern(v8Isolate, str)));
}

inline JsSymbol Lock::symbolShared(kj::StringPtr str) {
  return JsSymbol(v8::Symbol::For(v8Isolate, v8StrIntern(v8Isolate, str)));
}

inline JsSymbol Lock::symbolInternal(kj::StringPtr str) {
  return JsSymbol(v8::Symbol::ForApi(v8Isolate, v8StrIntern(v8Isolate, str)));
}

inline JsDate Lock::date(double timestamp) {
  return JsDate(check(v8::Date::New(v8Context(), timestamp)).As<v8::Date>());
}

inline JsDate Lock::date(kj::Date date) {
  return JsDate(jsg::check(v8::Date::New(v8Context(), (date - kj::UNIX_EPOCH) / kj::MILLISECONDS))
                    .As<v8::Date>());
}

inline void JsObject::set(Lock& js, const JsValue& name, const JsValue& value) {
  check(inner->Set(js.v8Context(), name.inner, value.inner));
}

inline void JsObject::set(Lock& js, kj::StringPtr name, const JsValue& value) {
  set(js, js.strIntern(name), value);
}

inline JsValue JsObject::get(Lock& js, const JsValue& name) {
  return JsValue(check(inner->Get(js.v8Context(), name.inner)));
}

inline JsValue JsObject::get(Lock& js, kj::StringPtr name) {
  return get(js, js.strIntern(name));
}

inline bool JsObject::has(Lock& js, const JsValue& name, HasOption option) {
  if (option == HasOption::OWN) {
    KJ_ASSERT(name.inner->IsName());
    return check(inner->HasOwnProperty(js.v8Context(), name.inner.As<v8::Name>()));
  } else {
    return check(inner->Has(js.v8Context(), name.inner));
  }
}

inline bool JsObject::has(Lock& js, kj::StringPtr name, HasOption option) {
  return has(js, js.strIntern(name), option);
}

inline void JsObject::delete_(Lock& js, const JsValue& name) {
  check(inner->Delete(js.v8Context(), name.inner));
}

inline void JsObject::delete_(Lock& js, kj::StringPtr name) {
  delete_(js, js.strIntern(name));
}

inline int JsString::length(jsg::Lock& js) const {
  return inner->Length();
}

inline size_t JsString::utf8Length(jsg::Lock& js) const {
  return inner->Utf8LengthV2(js.v8Isolate);
}

}  // namespace workerd::jsg
