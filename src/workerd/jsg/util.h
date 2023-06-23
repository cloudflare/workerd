// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// This file contains misc utility functions used elsewhere.

#include <kj/debug.h>
#include <kj/string.h>
#include <kj/exception.h>
#include <v8.h>
#include <typeinfo>

#include <workerd/util/sentry.h>

namespace workerd::jsg {

typedef unsigned int uint;

bool getCaptureThrowsAsRejections(v8::Isolate* isolate);
bool getCommonJsExportDefault(v8::Isolate* isolate);

kj::String fullyQualifiedTypeName(const std::type_info& type);
kj::String typeName(const std::type_info& type);

v8::Local<v8::Value> makeInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage);
v8::Local<v8::Value> makeInternalError(v8::Isolate* isolate, kj::Exception&& exception);
// Creates a JavaScript error that obfuscates the exception details, while logging the full details
// to stderr. If the KJ exception was created using throwTunneledException(), don't log anything
// but instead return the original reconstructed JavaScript exception.

void throwInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage);
void throwInternalError(v8::Isolate* isolate, kj::Exception&& exception);
// calls makeInternalError() and then tells the isolate to throw it.

struct TypeErrorContext {
  enum Kind: uint8_t {
    METHOD_ARGUMENT,      // has type name, member (method) name, and argument index
    CONSTRUCTOR_ARGUMENT, // has type name, argument index
    SETTER_ARGUMENT,      // has type name and member (property) name
    STRUCT_FIELD,         // has type name and member (field) name
    ARRAY_ELEMENT,        // has argument (element) index
                          // TODO(someday): Capture where the array itself was declared?
    CALLBACK_ARGUMENT,    // has argument index
                          // TODO(someday): Track where callback was introduced for better errors.
    CALLBACK_RETURN,      // has nothing
                          // TODO(someday): Track where callback was introduced for better errors.
    DICT_KEY,             // has member (key) name
    DICT_FIELD,           // has member (field) name
    PROMISE_RESOLUTION,   // has nothing
                          // TODO(someday): Track where the promise was introduced.
    OTHER,                // has nothing
  };

  Kind kind;
  uint argumentIndex;
  kj::Maybe<const std::type_info&> type;
  const char* memberName;

  static inline TypeErrorContext methodArgument(
      const std::type_info& type, const char* methodName, uint argumentIndex) {
    return { METHOD_ARGUMENT, argumentIndex, type, methodName };
  }
  static inline TypeErrorContext constructorArgument(
      const std::type_info& type, uint argumentIndex) {
    return { CONSTRUCTOR_ARGUMENT, argumentIndex, type, nullptr };
  }
  static inline TypeErrorContext setterArgument(
      const std::type_info& type, const char* propertyName) {
    return { SETTER_ARGUMENT, 0, type, propertyName };
  }
  static inline TypeErrorContext structField(const std::type_info& type, const char* fieldName) {
    return { STRUCT_FIELD, 0, type, fieldName };
  }
  static inline TypeErrorContext arrayElement(uint index) {
    return { ARRAY_ELEMENT, index, nullptr, nullptr };
  }
  static inline TypeErrorContext callbackArgument(uint argumentIndex) {
    return { CALLBACK_ARGUMENT, argumentIndex, nullptr, nullptr };
  }
  static inline TypeErrorContext callbackReturn() {
    return { CALLBACK_RETURN, 0, nullptr, nullptr };
  }
  static inline TypeErrorContext dictKey(const char* keyName) {
    return { DICT_KEY, 0, nullptr, keyName };
  }
  static inline TypeErrorContext dictField(const char* fieldName) {
    return { DICT_FIELD, 0, nullptr, fieldName };
  }
  static inline TypeErrorContext promiseResolution() {
    return { PROMISE_RESOLUTION, 0, nullptr, nullptr };
  }
  static inline TypeErrorContext other() {
    return { OTHER, 0, nullptr, nullptr };
  }
};

[[noreturn]] void throwTypeError(v8::Isolate* isolate,
    TypeErrorContext errorContext, const char* expectedType);
[[noreturn]] void throwTypeError(v8::Isolate* isolate,
    TypeErrorContext errorContext, const std::type_info& expectedType);
[[noreturn]] void throwTypeError(v8::Isolate* isolate,
    TypeErrorContext errorContext,
    kj::String expectedType);
// Throw a JavaScript exception indicating an argument type error, and then throw a C++ exception
// of type JsExceptionThrown, which will be caught by liftKj().

[[noreturn]] void throwTypeError(v8::Isolate* isolate, kj::StringPtr message);
// Throw a JavaScript TypeError with a free-form message.

void throwIllegalConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);
// Callback used when attempting to construct a type that can't be constructed from JavaScript.

kj::StringPtr extractTunneledExceptionDescription(kj::StringPtr message);

kj::Exception createTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception);
// Given a JavaScript exception, returns a KJ exception that contains a tunneled exception type that
// can be converted back to JavaScript via makeInternalError().

[[noreturn]] void throwTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception);
// Given a JavaScript exception, throw a KJ exception that contains a tunneled exception type that
// can be converted back to JavaScript via makeInternalError().
//
// Equivalent to throwing the exception returned by `createTunneledException(exception)`.

template <typename T>
v8::Local<T> check(v8::MaybeLocal<T> maybe) {
  // V8 usually returns a MaybeLocal to mean that the function can throw a JavaScript exception.
  // If the MaybeLocal is empty then an exception was already thrown.

  v8::Local<T> result;
  if (!maybe.ToLocal(&result)) {
    throw JsExceptionThrown();
  }
  return result;
}

template <typename T>
T check(v8::Maybe<T> maybe) {
  T result;
  if (maybe.To(&result)) {
    return result;
  } else {
    throw JsExceptionThrown();
  }
}

kj::Array<kj::byte> asBytes(v8::Local<v8::ArrayBuffer> arrayBuffer);
kj::Array<kj::byte> asBytes(v8::Local<v8::ArrayBufferView> arrayBufferView);
// View the contents of the given v8::ArrayBuffer/ArrayBufferView as an ArrayPtr<byte>.

void recursivelyFreeze(v8::Local<v8::Context> context, v8::Local<v8::Value> value);
// Freeze the given object and all its members, making it recursively immutable.
//
// WARNING: This function is unsafe to call on user-provided content since if the value is cyclic
//   or may contain non-simple values it won't do the right thing. It is safe to call this on the
//   output of JSON.parse().
// TODO(cleanup): Maybe replace this with a function that parses and freezes JSON in one step.

v8::Local<v8::Value> deepClone(v8::Local<v8::Context> context, v8::Local<v8::Value> value);
// Make a deep clone of the given object.

template <typename T>
v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::ArrayPtr<T> ptr,
           v8::NewStringType newType = v8::NewStringType::kNormal) {
  // Make a JavaScript String in v8's Heap.
  //
  // The type T of kj::Array<T> will determine the specific type of v8 string that is created:
  // * When T is const char, the kj::Array<T> is interpreted as UTF-8.
  // * When T is char16_t, the kj::Array<T> is interpreted as UTF-16.
  if constexpr (kj::isSameType<char16_t, T>()) {
    return check(v8::String::NewFromTwoByte(
        isolate,
        reinterpret_cast<uint16_t*>(ptr.begin()),
        newType,
        ptr.size()));
  } else if constexpr (kj::isSameType<const char16_t, T>()) {
    return check(v8::String::NewFromTwoByte(
        isolate,
        reinterpret_cast<const uint16_t*>(ptr.begin()),
        newType,
        ptr.size()));
  } else if constexpr (kj::isSameType<uint16_t, T>()) {
    return check(v8::String::NewFromTwoByte(isolate, ptr.begin(), newType, ptr.size()));
  } else if constexpr (kj::isSameType<const char, T>()) {
    return check(v8::String::NewFromUtf8(isolate, ptr.begin(), newType, ptr.size()));
  } else if constexpr (kj::isSameType<char, T>()) {
    return check(v8::String::NewFromUtf8(isolate, ptr.begin(), newType, ptr.size()));
  } else {
    KJ_UNREACHABLE;
  }
}

inline v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::StringPtr str,
           v8::NewStringType newType = v8::NewStringType::kNormal) {
  // Make a JavaScript String in v8's Heap with the kj::StringPtr interpreted as UTF-8.
  return v8Str(isolate, str.asArray(), newType);
}

inline v8::Local<v8::String> v8StrFromLatin1(
    v8::Isolate* isolate,
    kj::ArrayPtr<const kj::byte> ptr,
    v8::NewStringType newType = v8::NewStringType::kNormal) {
  // Make a JavaScript String in v8's Heap with the kj::ArrayPtr interpreted as Latin1.
  return check(v8::String::NewFromOneByte(isolate, ptr.begin(), newType, ptr.size()));
}

inline v8::Local<v8::String> v8StrIntern(v8::Isolate* isolate, kj::StringPtr str) {
  return v8Str(isolate, str, v8::NewStringType::kInternalized);
}

template <typename T> constexpr bool isVoid() { return false; }
template <> constexpr bool isVoid<void>() { return true; }

template <typename T> struct RemoveMaybe_;
template <typename T> struct RemoveMaybe_<kj::Maybe<T>> { typedef T Type; };
template <typename T> using RemoveMaybe = typename RemoveMaybe_<T>::Type;

template <typename T> struct RemoveRvalueRef_ { typedef T Type; };
template <typename T> struct RemoveRvalueRef_<T&&> { typedef T Type; };
template <typename T> using RemoveRvalueRef = typename RemoveRvalueRef_<T>::Type;

enum class JsgKind {
  RESOURCE,
  STRUCT,
  EXTENSION
};

template <typename T>
struct LiftKj_ {
  template <typename Info, typename Func>
  static void apply(const Info& info, Func&& func) {
    auto isolate = info.GetIsolate();
    try {
      try {
        v8::HandleScope scope(isolate);
        if constexpr(isVoid<T>()) {
          func();
          if constexpr (!kj::canConvert<Info&, v8::PropertyCallbackInfo<void>&>()) {
            info.GetReturnValue().SetUndefined();
          }
        } else {
          info.GetReturnValue().Set(func());
        }
      } catch (kj::Exception& exception) {
        // This throwInteralError() overload may decode a tunneled error. While constructing the
        // v8::Value representing the tunneled error, it itself may cause a JS exception to be
        // thrown. This is the reason for the nested try-catch blocks -- we need to be able to
        // swallow any JsExceptionThrowns that this catch block generates.
        throwInternalError(isolate, kj::mv(exception));
      }
    } catch (JsExceptionThrown&) {
      // nothing to do
    } catch (std::exception& exception) {
      throwInternalError(isolate, exception.what());
    } catch (...) {
      throwInternalError(isolate, kj::str("caught unknown exception of type: ",
                                          kj::getCaughtExceptionType()));
    }
  }
};

void returnRejectedPromise(
    const v8::FunctionCallbackInfo<v8::Value>& info,
    v8::Local<v8::Value> exception,
    v8::TryCatch& tryCatch);

void returnRejectedPromise(
    const v8::PropertyCallbackInfo<v8::Value>& info,
    v8::Local<v8::Value> exception,
    v8::TryCatch& tryCatch);

template <>
struct LiftKj_<v8::Local<v8::Promise>> {
  template <typename Info, typename Func>
  static void apply(Info& info, Func&& func) {
    auto isolate = info.GetIsolate();
    if (!getCaptureThrowsAsRejections(isolate)) {
      // Capturing exceptions into rejected promises is not enabled, so fall back to the regular
      // implementation.
      LiftKj_<v8::Local<v8::Value>>::apply(info, kj::fwd<Func>(func));
      return;
    }

    v8::HandleScope scope(isolate);
    v8::TryCatch tryCatch(isolate);
    try {
      try {
        info.GetReturnValue().Set(func());
      } catch (kj::Exception& exception) {
        // returnRejectedPromise() may decode a tunneled error. While constructing
        // the v8::Value representing the tunneled error, it itself may cause a JS exception to be
        // thrown. This is the reason for the nested try-catch blocks -- we need to be able to
        // swallow any JsExceptionThrowns that this catch block generates.
        returnRejectedPromise(info, makeInternalError(isolate, kj::mv(exception)), tryCatch);
      }
    } catch (JsExceptionThrown&) {
      if (tryCatch.CanContinue()) {
        returnRejectedPromise(info, tryCatch.Exception(), tryCatch);
      }
      // If CanContinue() is false, then there's nothing we can do.
    } catch (std::exception& exception) {
      throwInternalError(isolate, exception.what());
    } catch (...) {
      throwInternalError(isolate, kj::str("caught unknown exception of type: ",
                                          kj::getCaughtExceptionType()));
    }
  }
};

template <typename Info, typename Func>
void liftKj(const Info& info, Func&& func) {
  // Lifts KJ code into V8 code: Catches exceptions and manages HandleScope. Converts the
  // function's return value into the appropriate V8 return.
  //
  // liftKj() translates certain KJ exceptions thrown into JS exceptions. KJ exceptions opt into
  // this behavior by suffixing their description strings with "jsg.SomeError", followed by an
  // optional colon, optional whitespace, and a message which will be exposed to the user.
  // In the example "jsg.SomeError", "SomeError" must be a specific recognized string:
  // "RangeError", "TypeError", or "DOMException(name)", where `name` is the "error name" of the
  // DOMException you wish to throw.
  //
  // The DOMException error name will typically be dictated by the spec governing the API you are
  // implementing. While it can be any string without a closing parenthesis as far as JSG is
  // concerned, it will likely be one of the ones listed here:
  //
  // https://heycam.github.io/webidl/#dfn-error-names-table

  LiftKj_<decltype(func())>::apply(info, kj::fwd<Func>(func));
}

namespace _ {

struct NoneDetected;

template <typename Default, typename AlwaysVoid, template <typename...> class Op, class... Args>
struct Detector {
  using Type = Default;
  static constexpr bool value = false;
};

template <typename Default, template <typename...> class Op, class... Args>
struct Detector<Default, kj::VoidSfinae<Op<Args...>>, Op, Args...> {
  using Type = Op<Args...>;
  static constexpr bool value = true;
};

}  // namespace _ (private)

template <typename Default, template <typename...> class Op, typename... Args>
using DetectedOr = typename _::Detector<Default, void, Op, Args...>::Type;
// A typedef for `Op<Args...>` if that template is instantiable, otherwise `Default`.

template <template <typename...> class Op, typename... Args>
constexpr bool isDetected() { return _::Detector<_::NoneDetected, void, Op, Args...>::value; }
// True if Op<Args...> is instantiable, false otherwise. This is basically the same as
// std::experimental::is_detected from the library fundamentals TS v2.
//   http://en.cppreference.com/w/cpp/experimental/is_detected
//
// TODO(cleanup): Should live in kj?

template <typename Arg> auto getParameterType(void (*)(Arg)) -> Arg;
template <typename T> using GetConfiguration = decltype(getParameterType(&T::jsgConfiguration));
// SFINAE-friendly accessor for a resource type's configuration parameter.

v8::Local<v8::Value> makeDOMException(
    v8::Isolate* isolate,
    v8::Local<v8::String> message,
    kj::StringPtr name);

inline bool isFinite(double value) {
  return !(kj::isNaN(value) || value == kj::inf() || value == -kj::inf());
}

// ======================================================================================

class Lock;

v8::Local<v8::String> newExternalOneByteString(Lock& js, kj::ArrayPtr<const char> buf);
v8::Local<v8::String> newExternalTwoByteString(Lock& js, kj::ArrayPtr<const uint16_t> buf);
// Creates v8 Strings from buffers not on the v8 heap. These do not copy and do not
// take ownership of the buf. The buf *must* point to a static constant with infinite
// lifetime.
//
// It is important to understand that the OneByteString variant will interpret buf as
// latin-1 rather than UTF-8, which is how KJ normally represents text. There is no
// variation of external strings that support UTF-8 encoded bytes. To represent any
// text outside of the Latin1 range, the two-byte (uint16_t) variant must be used.
//
// Note that these intentionally do not use the v8Str naming convention like the other
// string methods because it needs to be absolutely clear that these use external buffers
// that are not owned by the v8 heap.

}  // namespace workerd::jsg
