// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"  // can't include util.h directly due to weird cyclic dependency...
#include "ser.h"
#include "setup.h"

#include <kj/debug.h>

#include <cstdlib>
#include <set>

#if !_WIN32
#include <cxxabi.h>
#endif

#include <workerd/util/sentry.h>

namespace workerd::jsg {

bool getCaptureThrowsAsRejections(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
  return jsgIsolate.getCaptureThrowsAsRejections();
}

bool getCommonJsExportDefault(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
  return jsgIsolate.getCommonJsExportDefault();
}

bool getShouldSetToStringTag(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
  return jsgIsolate.shouldSetToStringTag();
}

#if _WIN32
kj::String fullyQualifiedTypeName(const std::type_info& type) {
  // type.name() returns a human-readable name on Windows:
  // https://learn.microsoft.com/en-us/cpp/cpp/type-info-class?view=msvc-170
  kj::StringPtr name = type.name();

  // Remove struct prefix
  if (name.startsWith("struct ")) {
    name = name.slice(7);
  }
  // Remove class prefix
  if (name.startsWith("class ")) {
    name = name.slice(6);
  }

  kj::String result = kj::str(name);

  // Replace instances of `anonymous namespace' with (anonymous namespace)
  for (auto& c: result.asArray()) {
    if (c == '`')
      c = '(';
    else if (c == '\'')
      c = ')';
  }

  return kj::mv(result);
}
#else
kj::String fullyQualifiedTypeName(const std::type_info& type) {
  int status;
  char* buf = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
  kj::String result = kj::str(buf == nullptr ? type.name() : buf);
  free(buf);

  return kj::mv(result);
}
#endif

kj::String typeName(const std::type_info& type) {
  auto result = fullyQualifiedTypeName(type);

  // Strip namespace, if any.
  KJ_IF_SOME(pos, result.findLast(':')) {
    result = kj::str(result.slice(pos + 1));
  }

  // Strip template args, if any.
  //
  // TODO(someday): Maybe just strip namespaces from each arg?
  KJ_IF_SOME(pos, result.findFirst('<')) {
    result = kj::str(result.first(pos));
  }

  return kj::mv(result);
}

v8::Local<v8::Value> makeInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage) {
  KJ_LOG(ERROR, internalMessage);
  return v8::Exception::Error(v8StrIntern(isolate, "internal error"));
}

namespace {

kj::StringPtr trimErrorMessage(kj::StringPtr errorString) {
  // For strings beginning with ':' OWS, returns everything after the OWS. Otherwise returns the
  // empty string.
  if (errorString.startsWith(":")) {
    errorString = errorString.slice(1);
    while (errorString.startsWith(" ")) {
      errorString = errorString.slice(1);
    }
    return errorString;
  }
  return "";
}

bool setRemoteError(v8::Isolate* isolate, v8::Local<v8::Value>& exception) {
  // If an exception was tunneled, we add a property `.remote` to the Javascript error.
  KJ_ASSERT(exception->IsObject());
  auto obj = exception.As<v8::Object>();
  return jsg::check(obj->Set(
      isolate->GetCurrentContext(), jsg::v8StrIntern(isolate, "remote"_kj), v8::True(isolate)));
}

bool setRetryableError(v8::Isolate* isolate, v8::Local<v8::Value>& exception) {
  KJ_ASSERT(exception->IsObject());
  auto obj = exception.As<v8::Object>();
  return jsg::check(obj->Set(
      isolate->GetCurrentContext(), jsg::v8StrIntern(isolate, "retryable"_kj), v8::True(isolate)));
}

bool setOverloadedError(v8::Isolate* isolate, v8::Local<v8::Value>& exception) {
  KJ_ASSERT(exception->IsObject());
  auto obj = exception.As<v8::Object>();
  return jsg::check(obj->Set(
      isolate->GetCurrentContext(), jsg::v8StrIntern(isolate, "overloaded"_kj), v8::True(isolate)));
}

bool setDurableObjectResetError(v8::Isolate* isolate, v8::Local<v8::Value>& exception) {
  KJ_ASSERT(exception->IsObject());
  auto obj = exception.As<v8::Object>();
  return jsg::check(obj->Set(isolate->GetCurrentContext(),
      jsg::v8StrIntern(isolate, "durableObjectReset"_kj), v8::True(isolate)));
}
struct DecodedException {
  v8::Local<v8::Value> handle;
  bool isInternal;
  bool isFromRemote;
  bool isDurableObjectReset;
};

DecodedException decodeTunneledException(
    v8::Isolate* isolate, kj::StringPtr internalMessage, kj::Exception::Type excType) {
  // We currently support tunneling the following error types:
  //
  // - Error:        While the Web IDL spec claims this is reserved for use by program authors, this
  //                 is broadly useful as a general-purpose error type.
  // - RangeError:   Commonly thrown by web API implementations.
  // - TypeError:    Commonly thrown by web API implementations.
  // - SyntaxError:  Especially from JSON parsing.
  // - ReferenceError: Not thrown by our APIs, but could be tunneled from user code.
  // - DOMException: Commonly thrown by web API implementations.
  //
  // ECMA-262 additionally defines EvalError and URIError, but V8 doesn't provide any API to
  // construct them.
  //
  // Note that this list is also present below in `tunneledErrorPrefixes()`.
  //
  // https://heycam.github.io/webidl/#idl-exceptions
  //
  // TODO(someday): Support arbitrary user-defined error types, not just Error?
  auto tunneledInfo = tunneledErrorType(internalMessage);

  auto errorType = tunneledInfo.message;
  auto appMessage = [&](kj::StringPtr errorString) -> kj::StringPtr {
    if (tunneledInfo.isInternal) {
      return "internal error"_kj;
    } else {
      return trimErrorMessage(errorString);
    }
  };
  DecodedException result;
  result.isInternal = tunneledInfo.isInternal;
  result.isFromRemote = tunneledInfo.isFromRemote;
  result.isDurableObjectReset = tunneledInfo.isDurableObjectReset;

#define HANDLE_V8_ERROR(error_name, error_type)                                                    \
  if (errorType.startsWith(error_name)) {                                                          \
    auto message = appMessage(errorType.slice(strlen(error_name)));                                \
    result.handle = v8::Exception::error_type(v8Str(isolate, message));                            \
    break;                                                                                         \
  }

  do {
    if (tunneledInfo.isJsgError) {
      HANDLE_V8_ERROR("Error", Error);
      HANDLE_V8_ERROR("RangeError", RangeError);
      HANDLE_V8_ERROR("TypeError", TypeError);
      HANDLE_V8_ERROR("SyntaxError", SyntaxError);
      HANDLE_V8_ERROR("ReferenceError", ReferenceError);
      HANDLE_V8_ERROR("CompileError", WasmCompileError);
      HANDLE_V8_ERROR("LinkError", WasmCompileError);
      HANDLE_V8_ERROR("RuntimeError", WasmCompileError);

      // DOMExceptions require a parenthesized error name argument, like DOMException(SyntaxError).
      if (errorType.startsWith("DOMException(")) {
        errorType = errorType.slice(strlen("DOMException("));
        // Check for closing brace
        KJ_IF_SOME(closeParen, errorType.findFirst(')')) {
          auto& js = Lock::from(isolate);
          auto errorName = kj::str(errorType.first(closeParen));
          auto message = appMessage(errorType.slice(1 + closeParen));
          auto exception = js.domException(kj::mv(errorName), kj::str(message));
          result.handle = KJ_ASSERT_NONNULL(exception.tryGetHandle(js));
          break;
        }
      }
    }
    // unrecognized exception type
    result.handle = v8::Exception::Error(v8StrIntern(isolate, "internal error"));
    result.isInternal = true;
  } while (false);
#undef HANDLE_V8_ERROR

  if (result.isFromRemote) {
    setRemoteError(isolate, result.handle);
  }

  if (excType == kj::Exception::Type::DISCONNECTED) {
    setRetryableError(isolate, result.handle);
  } else if (excType == kj::Exception::Type::OVERLOADED) {
    setOverloadedError(isolate, result.handle);
  }

  if (result.isDurableObjectReset) {
    setDurableObjectResetError(isolate, result.handle);
  }

  return result;
}

}  // namespace

kj::StringPtr extractTunneledExceptionDescription(kj::StringPtr message) {
  auto tunneledError = tunneledErrorType(message);
  if (tunneledError.isInternal) {
    return "Error: internal error";
  } else {
    return tunneledError.message;
  }
}

v8::Local<v8::Value> makeInternalError(v8::Isolate* isolate, kj::Exception&& exception) {
  auto desc = exception.getDescription();

  // TODO(someday): Deserialize encoded V8 exception from
  //   exception.getDetail(TUNNELED_EXCEPTION_DETAIL_ID), if present. WARNING: We must think
  //   carefully about security in the case that the exception has passed between workers that
  //   don't trust each other. Perhaps we should explicitly remove the stack trace in this case.
  //   REMINDER: Worker::logUncaughtException() currently deserializes TUNNELED_EXCEPTION_DETAIL_ID
  //   in order to extract a full stack trace. Once we do it here, we can remove the code from
  //   there.

  auto tunneledException = decodeTunneledException(isolate, desc, exception.getType());

  if (tunneledException.isInternal) {
    auto& observer = IsolateBase::from(isolate).getObserver();
    observer.reportInternalException(exception,
        {
          .isInternal = tunneledException.isInternal,
          .isFromRemote = tunneledException.isFromRemote,
          .isDurableObjectReset = tunneledException.isDurableObjectReset,
        });
    // Don't log exceptions that have been explicitly marked with worker_do_not_log or are
    // DISCONNECTED exceptions as these are unlikely to represent bugs worth tracking.
    if (exception.getType() != kj::Exception::Type::DISCONNECTED &&
        !isDoNotLogException(exception.getDescription())) {
      LOG_EXCEPTION("jsgInternalError", exception);
    } else {
      KJ_LOG(INFO, exception);  // Run with --verbose to see exception logs.
    }

    if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
      auto exception = v8::Exception::Error(v8StrIntern(isolate, "Network connection lost."_kj));
      if (tunneledException.isFromRemote) {
        setRemoteError(isolate, exception);
      }

      // DISCONNECTED exceptions are considered retryable
      setRetryableError(isolate, exception);

      if (tunneledException.isDurableObjectReset) {
        setDurableObjectResetError(isolate, exception);
      }

      return exception;
    }
  }

  return tunneledException.handle;
}

Value Lock::exceptionToJs(kj::Exception&& exception) {
  return withinHandleScope(
      [&] { return Value(v8Isolate, makeInternalError(v8Isolate, kj::mv(exception))); });
}

JsRef<JsValue> Lock::exceptionToJsValue(kj::Exception&& exception) {
  return withinHandleScope([&] {
    JsValue val = JsValue(makeInternalError(v8Isolate, kj::mv(exception)));
    return val.addRef(*this);
  });
}

void Lock::throwException(Value&& exception) {
  withinHandleScope([&] { v8Isolate->ThrowException(exception.getHandle(*this)); });
  throw JsExceptionThrown();
}

void Lock::throwException(const JsValue& exception) {
  withinHandleScope([&] { v8Isolate->ThrowException(exception); });
  throw JsExceptionThrown();
}

void throwInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage) {
  isolate->ThrowException(makeInternalError(isolate, internalMessage));
}

void throwInternalError(v8::Isolate* isolate, kj::Exception&& exception) {
  KJ_IF_SOME(renderingError, kj::runCatchingExceptions([&]() {
    isolate->ThrowException(makeInternalError(isolate, kj::mv(exception)));
  })) {
    KJ_LOG(ERROR, "error rendering exception", renderingError);
    KJ_LOG(ERROR, exception);
    throwInternalError(isolate, "error rendering exception");
  }
}

void addExceptionDetail(Lock& js, kj::Exception& exception, v8::Local<v8::Value> handle) {
  v8::TryCatch tryCatch(js.v8Isolate);
  try {
    Serializer ser(js,
        {
          // Make sure we don't break compatibility if V8 introduces a new version. This value can
          // be bumped to match the new version once all of production is updated to understand it.
          .version = 15,
        });
    ser.write(js, JsValue(handle));
    exception.setDetail(TUNNELED_EXCEPTION_DETAIL_ID, ser.release().data);
  } catch (JsExceptionThrown&) {
    // Either:
    // a. The exception is not serializable, and we caught the exception. We will just ignore it
    //    and proceed without annotating.
    // b. The isolate's execution is being terminated, and so tryCatch.CanContinue() is false. In
    //    this case we cannot serialize the exception, but again we'll just move on without the
    //    annotation.
  }
}

static kj::String typeErrorMessage(TypeErrorContext c, const char* expectedType) {
  kj::String type;

  KJ_IF_SOME(t, c.type) {
    type = typeName(t);
  }

  switch (c.kind) {
    case TypeErrorContext::METHOD_ARGUMENT:
      return kj::str("Failed to execute '", c.memberName, "' on '", type, "': parameter ",
          c.argumentIndex + 1, " is not of type '", expectedType, "'.");
    case TypeErrorContext::CONSTRUCTOR_ARGUMENT:
      return kj::str("Failed to construct '", type, "': constructor parameter ",
          c.argumentIndex + 1, " is not of type '", expectedType, "'.");
    case TypeErrorContext::SETTER_ARGUMENT:
      return kj::str("Failed to set the '", c.memberName, "' property on '", type,
          "': the provided value is not of type '", expectedType, "'.");
    case TypeErrorContext::STRUCT_FIELD:
      return kj::str("Incorrect type for the '", c.memberName, "' field on '", type,
          "': the provided value is not of type '", expectedType, "'.");
    case TypeErrorContext::ARRAY_ELEMENT:
      return kj::str("Incorrect type for array element ", c.argumentIndex,
          ": the provided value is not of type '", expectedType, "'.");
    case TypeErrorContext::CALLBACK_ARGUMENT:
      return kj::str("Failed to execute function: parameter ", c.argumentIndex + 1,
          " is not of type '", expectedType, "'.");
    case TypeErrorContext::CALLBACK_RETURN:
      return kj::str("Callback returned incorrect type; expected '", expectedType, "'");
    case TypeErrorContext::DICT_KEY:
      return kj::str("Incorrect type for map entry '", c.memberName,
          "': the provided key is not of type '", expectedType, "'.");
    case TypeErrorContext::DICT_FIELD:
      return kj::str("Incorrect type for map entry '", c.memberName,
          "': the provided value is not of type '", expectedType, "'.");
    case TypeErrorContext::PROMISE_RESOLUTION:
      return kj::str(
          "Incorrect type for Promise: the Promise did not resolve to '", expectedType, "'.");
    case TypeErrorContext::OTHER:
      return kj::str("Incorrect type: the provided value is not of type '", expectedType, "'.");
  };

  KJ_UNREACHABLE;
}

static kj::String unimplementedErrorMessage(TypeErrorContext c) {
  kj::String type;

  KJ_IF_SOME(t, c.type) {
    type = typeName(t);
  }

  switch (c.kind) {
    case TypeErrorContext::METHOD_ARGUMENT:
      return kj::str("Failed to execute '", c.memberName, "' on '", type, "': parameter ",
          c.argumentIndex + 1, " is not implemented.");
    case TypeErrorContext::CONSTRUCTOR_ARGUMENT:
      return kj::str("Failed to construct '", type, "': constructor parameter ",
          c.argumentIndex + 1, " is not implemented.");
    case TypeErrorContext::SETTER_ARGUMENT:
      return kj::str("Failed to set the '", c.memberName, "' property on '", type,
          "': the ability to set this property is not implemented.");
    case TypeErrorContext::STRUCT_FIELD:
      return kj::str("The '", c.memberName, "' field on '", type, "' is not implemented.");
    case TypeErrorContext::ARRAY_ELEMENT:
      KJ_UNREACHABLE;
    case TypeErrorContext::CALLBACK_ARGUMENT:
      return kj::str(
          "Failed to execute function: parameter ", c.argumentIndex + 1, " is not implemented.");
    case TypeErrorContext::CALLBACK_RETURN:
      KJ_UNREACHABLE;
    case TypeErrorContext::DICT_KEY:
      KJ_UNREACHABLE;
    case TypeErrorContext::DICT_FIELD:
      KJ_UNREACHABLE;
    case TypeErrorContext::PROMISE_RESOLUTION:
      KJ_UNREACHABLE;
    case TypeErrorContext::OTHER:
      KJ_UNREACHABLE;
  };

  KJ_UNREACHABLE;
}

void throwTypeError(v8::Isolate* isolate, kj::StringPtr message) {
  isolate->ThrowException(v8::Exception::TypeError(v8Str(isolate, message)));
  throw JsExceptionThrown();
}

void throwTypeError(v8::Isolate* isolate, TypeErrorContext errorContext, kj::String expectedType) {
  kj::String message = typeErrorMessage(errorContext, expectedType.cStr());
  throwTypeError(isolate, message);
}

void throwTypeError(v8::Isolate* isolate, TypeErrorContext errorContext, const char* expectedType) {
  kj::String message = typeErrorMessage(errorContext, expectedType);
  throwTypeError(isolate, message);
}

void throwTypeError(
    v8::Isolate* isolate, TypeErrorContext errorContext, const std::type_info& expectedType) {
  if (expectedType == typeid(Unimplemented)) {
    isolate->ThrowError(v8StrIntern(isolate, unimplementedErrorMessage(errorContext)));
    throw JsExceptionThrown();
  } else {
    throwTypeError(isolate, errorContext, typeName(expectedType).cStr());
  }
}

static constexpr auto kIllegalConstructorMessage = "Illegal constructor";

void throwIllegalConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto isolate = args.GetIsolate();
  isolate->ThrowException(
      v8::Exception::TypeError(v8StrIntern(isolate, kIllegalConstructorMessage)));
}

void throwTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception) {
  kj::throwFatalException(createTunneledException(isolate, exception));
}

kj::Exception createTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
  return jsgIsolate.unwrapException(isolate->GetCurrentContext(), exception);
}

kj::Exception Lock::exceptionToKj(Value&& exception) {
  return withinHandleScope(
      [&] { return createTunneledException(v8Isolate, exception.getHandle(*this)); });
}

kj::Exception Lock::exceptionToKj(const JsValue& exception) {
  return withinHandleScope([&] { return createTunneledException(v8Isolate, exception); });
}

static kj::byte DUMMY = 0;
static kj::Array<kj::byte> getEmptyArray() {
  // An older version of asBytes(), when given an empty ArrayBuffer, would often return an array
  // with zero size but non-empty start address. Meanwhile, it turns out that some code,
  // particularly in BoringSSL, does not like receiving a null pointer even when the length is
  // zero -- it will spuriously produce an error. We could carefully find all the places where
  // this is an issue and adjust the specific calls to avoid passing null pointers, but it is
  // easier to change `asBytes()` so that it never produces a null start address in the first
  // place.
  return kj::Array<kj::byte>(&DUMMY, 0, kj::NullArrayDisposer::instance);
}

kj::Array<kj::byte> asBytes(v8::Local<v8::ArrayBuffer> arrayBuffer) {
  auto backing = arrayBuffer->GetBackingStore();
  kj::ArrayPtr bytes(static_cast<kj::byte*>(backing->Data()), backing->ByteLength());
  if (bytes == nullptr) {
    return getEmptyArray();
  } else {
    return bytes.attach(kj::mv(backing));
  }
}
kj::Array<kj::byte> asBytes(v8::Local<v8::ArrayBufferView> arrayBufferView) {
  auto backing = arrayBufferView->Buffer()->GetBackingStore();
  kj::ArrayPtr buffer(static_cast<kj::byte*>(backing->Data()), backing->ByteLength());
  auto sliceStart = arrayBufferView->ByteOffset();
  auto sliceEnd = sliceStart + arrayBufferView->ByteLength();
  KJ_ASSERT(buffer.size() >= sliceEnd);
  auto bytes = buffer.slice(sliceStart, sliceEnd);
  if (bytes == nullptr) {
    return getEmptyArray();
  } else {
    return bytes.attach(kj::mv(backing));
  }
}

void recursivelyFreeze(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  if (value->IsArray()) {
    // Optimize array freezing (Array is a subclass of Object, but we can iterate it faster).
    v8::HandleScope scope(context->GetIsolate());
    auto arr = value.As<v8::Array>();

    for (auto i: kj::zeroTo(arr->Length())) {
      recursivelyFreeze(context, check(arr->Get(context, i)));
    }

    check(arr->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen));
  } else if (value->IsObject()) {
    v8::HandleScope scope(context->GetIsolate());
    auto obj = value.As<v8::Object>();
    auto names = check(obj->GetPropertyNames(context, v8::KeyCollectionMode::kOwnOnly,
        v8::ALL_PROPERTIES, v8::IndexFilter::kIncludeIndices));

    for (auto i: kj::zeroTo(names->Length())) {
      recursivelyFreeze(context, check(obj->Get(context, check(names->Get(context, i)))));
    }

    check(obj->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen));
  } else {
    // Primitive type, nothing to do.
  }
}

v8::Local<v8::Value> deepClone(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  // This is implemented in the classic JSON restringification way.
  auto serialized = check(v8::JSON::Stringify(context, value));
  return check(v8::JSON::Parse(context, serialized));
}

namespace {
v8::MaybeLocal<v8::Value> makeRejectedPromise(
    v8::Isolate* isolate, v8::Local<v8::Value> exception) {
  v8::Local<v8::Promise::Resolver> resolver;
  auto context = isolate->GetCurrentContext();
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
      resolver->Reject(context, exception).IsNothing()) {
    return v8::MaybeLocal<v8::Value>();
  }

  return resolver->GetPromise();
};

void returnRejectedPromiseImpl(auto info, v8::Local<v8::Value> exception, v8::TryCatch& tryCatch) {
  v8::Local<v8::Value> promise;
  if (!makeRejectedPromise(info.GetIsolate(), exception).ToLocal(&promise)) {
    // If makeRejectedPromise fails, the tryCatch should have caught the error.
    // Let's rethrow it if it isn't terminal.
    if (tryCatch.CanContinue()) tryCatch.ReThrow();
  }
  info.GetReturnValue().Set(promise);
}
}  // namespace

void returnRejectedPromise(const v8::FunctionCallbackInfo<v8::Value>& info,
    v8::Local<v8::Value> exception,
    v8::TryCatch& tryCatch) {
  return returnRejectedPromiseImpl<const v8::FunctionCallbackInfo<v8::Value>&>(
      info, exception, tryCatch);
}

void returnRejectedPromise(const v8::PropertyCallbackInfo<v8::Value>& info,
    v8::Local<v8::Value> exception,
    v8::TryCatch& tryCatch) {
  return returnRejectedPromiseImpl<const v8::PropertyCallbackInfo<v8::Value>&>(
      info, exception, tryCatch);
}

// ======================================================================================

template <typename Type, typename Data>
class ExternString: public Type {
  // The implementation of ExternString here is very closely after the implementation of the same
  // class in Node.js, with modifications to fit our conventions. It is distributed under the
  // same MIT license that Node.js uses. The appropriate copyright attribution is included here:
  //
  // Copyright Node.js contributors. All rights reserved.

  // Permission is hereby granted, free of charge, to any person obtaining a copy
  // of this software and associated documentation files (the "Software"), to
  // deal in the Software without restriction, including without limitation the
  // rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
  // sell copies of the Software, and to permit persons to whom the Software is
  // furnished to do so, subject to the following conditions:

  // The above copyright notice and this permission notice shall be included in
  // all copies or substantial portions of the Software.

  // THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  // IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  // FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  // AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  // LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  // FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  // IN THE SOFTWARE.

 public:
  inline const Data* data() const override {
    return buf.begin();
  }

  inline size_t length() const override {
    return buf.size();
  }

  inline uint64_t byteLength() const {
    return length() * sizeof(Data);
  }

  static v8::MaybeLocal<v8::String> createExtern(
      v8::Isolate* isolate, kj::ArrayPtr<const Data>& buf) {
    if (buf.size() == 0) {
      return v8::String::Empty(isolate);
    }

    // TODO(now): In Node.js impl, we check to see if length is less than a specified
    // minimum. If it is, it's likely more efficient to just copy and use a regular
    // heap allocated string than an external. We're not doing that here currently, but
    // we might?

    // We typically don't use the new keyword in workerd/Workers but in this case we
    // have to.
    auto resource = new ExternString<Type, Data>(isolate, buf);

    v8::MaybeLocal<v8::String> str;
    if constexpr (kj::isSameType<Type, v8::String::ExternalOneByteStringResource>()) {
      str = v8::String::NewExternalOneByte(isolate, resource);
    } else {
      // resource here must be a v8::String::ExternalStringResource.
      str = v8::String::NewExternalTwoByte(isolate, resource);
    }
    if (str.IsEmpty()) {
      // This should happen only if the string is too long
      delete resource;
      return v8::MaybeLocal<v8::String>();
    }

    return str;
  }

 private:
  v8::Isolate* isolate;
  kj::ArrayPtr<const Data> buf;

  inline ExternString(v8::Isolate* isolate, kj::ArrayPtr<const Data>& buf)
      : isolate(isolate),
        buf(buf) {}
};

using ExternOneByteString = ExternString<v8::String::ExternalOneByteStringResource, char>;
using ExternTwoByteString = ExternString<v8::String::ExternalStringResource, uint16_t>;

v8::Local<v8::String> newExternalOneByteString(Lock& js, kj::ArrayPtr<const char> buf) {
  return check(ExternOneByteString::createExtern(js.v8Isolate, buf));
}

v8::Local<v8::String> newExternalTwoByteString(Lock& js, kj::ArrayPtr<const uint16_t> buf) {
  return check(ExternTwoByteString::createExtern(js.v8Isolate, buf));
}

// ======================================================================================
// Node.js Compat

namespace {
// This list must be kept in sync with the list of builtins from Node.js.
// It should be unlikely that anything is ever removed from this list, and
// adding items to it is considered a semver-major change in Node.js.
static const std::set<kj::StringPtr> NODEJS_BUILTINS{"_http_agent"_kj, "_http_client"_kj,
  "_http_common"_kj, "_http_incoming"_kj, "_http_outgoing"_kj, "_http_server"_kj,
  "_stream_duplex"_kj, "_stream_passthrough"_kj, "_stream_readable"_kj, "_stream_transform"_kj,
  "_stream_wrap"_kj, "_stream_writable"_kj, "_tls_common"_kj, "_tls_wrap"_kj, "assert"_kj,
  "assert/strict"_kj, "async_hooks"_kj, "buffer"_kj, "child_process"_kj, "cluster"_kj, "console"_kj,
  "constants"_kj, "crypto"_kj, "dgram"_kj, "diagnostics_channel"_kj, "dns"_kj, "dns/promises"_kj,
  "domain"_kj, "events"_kj, "fs"_kj, "fs/promises"_kj, "http"_kj, "http2"_kj, "https"_kj,
  "inspector"_kj, "inspector/promises"_kj, "module"_kj, "net"_kj, "os"_kj, "path"_kj,
  "path/posix"_kj, "path/win32"_kj, "perf_hooks"_kj, "process"_kj, "punycode"_kj, "querystring"_kj,
  "readline"_kj, "readline/promises"_kj, "repl"_kj, "stream"_kj, "stream/consumers"_kj,
  "stream/promises"_kj, "stream/web"_kj, "string_decoder"_kj, "sys"_kj, "timers"_kj,
  "timers/promises"_kj, "tls"_kj, "trace_events"_kj, "tty"_kj, "url"_kj, "util"_kj, "util/types"_kj,
  "v8"_kj, "vm"_kj, "wasi"_kj, "worker_threads"_kj, "zlib"_kj};
}  // namespace

kj::Maybe<kj::String> checkNodeSpecifier(kj::StringPtr specifier) {
  // The sys module was renamed to 'util'. This shim remains to keep old programs
  // working. `sys` is deprecated and shouldn't be used.
  // Note to maintainers: Although this module has been deprecated for a while
  // Node.js do not plan to remove it.
  // See: https://github.com/nodejs/node/pull/35407#issuecomment-700693439
  if (specifier == "sys" || specifier == "node:sys") [[unlikely]] {
    return kj::str("node:util");
  }
  if (NODEJS_BUILTINS.contains(specifier)) {
    return kj::str("node:", specifier);
  } else if (specifier.startsWith("node:")) {
    return kj::str(specifier);
  }
  return kj::none;
}

bool isNodeJsCompatEnabled(jsg::Lock& js) {
  return IsolateBase::from(js.v8Isolate).isNodeJsCompatEnabled();
}

}  // namespace workerd::jsg
