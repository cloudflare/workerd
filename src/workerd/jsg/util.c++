// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"
#include "jsg.h"  // can't include util.h directly due to weird cyclic dependency...
#include "ser.h"
#include "setup.h"

#include <workerd/jsg/exception-metadata.capnp.h>

#include <simdutf.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/debug.h>

#include <cstdlib>
#include <new>
#include <set>

#if !_WIN32
#include <cxxabi.h>
#endif

namespace workerd::jsg {

CodeCacheKey codeCacheKeyForModule(StaticExternalStringSource source) {
  KJ_SWITCH_ONEOF(source) {
    KJ_CASE_ONEOF(oneByte, kj::ArrayPtr<const char>) {
      return CodeCacheKey::compute(CodeCacheKey::Unit::ONE_BYTE_ESM, oneByte);
    }
    KJ_CASE_ONEOF(twoByte, kj::ArrayPtr<const uint16_t>) {
      return CodeCacheKey::compute(CodeCacheKey::Unit::TWO_BYTE_ESM, twoByte.asBytes().asChars());
    }
  }
  KJ_UNREACHABLE;
}

CodeCacheKey codeCacheKeyForModule(const ExternalStringSource& source) {
  KJ_SWITCH_ONEOF(source) {
    KJ_CASE_ONEOF(oneByte, kj::ArrayPtr<const char>) {
      return CodeCacheKey::compute(CodeCacheKey::Unit::ONE_BYTE_ESM, oneByte);
    }
    KJ_CASE_ONEOF(twoByte, kj::ArrayPtr<const uint16_t>) {
      return CodeCacheKey::compute(CodeCacheKey::Unit::TWO_BYTE_ESM, twoByte.asBytes().asChars());
    }
    KJ_CASE_ONEOF(oneByte, kj::Arc<OwnedAscii>) {
      return CodeCacheKey::compute(CodeCacheKey::Unit::ONE_BYTE_ESM, *oneByte);
    }
    KJ_CASE_ONEOF(twoByte, kj::Arc<OwnedUtf16>) {
      return CodeCacheKey::compute(CodeCacheKey::Unit::TWO_BYTE_ESM, twoByte->asBytes().asChars());
    }
  }
  KJ_UNREACHABLE;
}

bool getCaptureThrowsAsRejections(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(SET_DATA_ISOLATE_BASE));
  return jsgIsolate.getCaptureThrowsAsRejections();
}

bool getShouldSetToStringTag(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(SET_DATA_ISOLATE_BASE));
  return jsgIsolate.shouldSetToStringTag();
}

bool getShouldSetImmutablePrototype(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(SET_DATA_ISOLATE_BASE));
  return jsgIsolate.shouldSetImmutablePrototype();
}

bool getSpecCompliantPropertyAttributes(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(SET_DATA_ISOLATE_BASE));
  return jsgIsolate.shouldUseSpecCompliantPropertyAttributes();
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
    auto unqualified = kj::str(result.slice(pos + 1));
    result = kj::mv(unqualified);
  }

  // Strip template args, if any.
  //
  // TODO(someday): Maybe just strip namespaces from each arg?
  KJ_IF_SOME(pos, result.findFirst('<')) {
    auto withoutTemplateArgs = kj::str(result.first(pos));
    result = kj::mv(withoutTemplateArgs);
  }

  return kj::mv(result);
}

kj::String latin1ToUtf8(kj::ArrayPtr<const char> latin1) {
  size_t utf8Length = simdutf::utf8_length_from_latin1(latin1.begin(), latin1.size());
  kj::String out = kj::heapString(utf8Length);
  if (utf8Length == latin1.size()) {
    // Pure ASCII, so latin1 and UTF-8 match.
    out.asArray().copyFrom(latin1);
  } else {
    [[maybe_unused]] size_t actualLength =
        simdutf::convert_latin1_to_utf8(latin1.begin(), latin1.size(), out.begin());
    KJ_DASSERT(actualLength == utf8Length);
  }
  return out;
}

namespace {

kj::String renderInternalError(InternalErrorId& internalErrorId) {
  return kj::str("internal error; reference = ", internalErrorId);
}

}  // namespace

v8::Local<v8::Value> makeInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage) {
  auto wdErrId = makeInternalErrorId();
  KJ_LOG(ERROR, internalMessage, wdErrId);
  return v8::Exception::Error(v8Str(isolate, renderInternalError(wdErrId)));
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

bool setDurableObjectIdError(
    v8::Isolate* isolate, v8::Local<v8::Value>& exception, kj::StringPtr durableObjectId) {
  KJ_ASSERT(exception->IsObject());
  auto obj = exception.As<v8::Object>();
  return jsg::check(obj->Set(isolate->GetCurrentContext(),
      jsg::v8StrIntern(isolate, "durableObjectId"_kj), jsg::v8Str(isolate, durableObjectId)));
}

constexpr kj::StringPtr NETWORK_CONNECTION_LOST = "Network connection lost."_kj;

// What decoding a kj::Exception determines besides the JavaScript error itself.
struct DecodedExceptionInfo {
  bool isInternal;
  bool isFromRemote;
  bool isDurableObjectReset;
  // TODO(cleanup): Maybe<> is redundant with isInternal flag field?
  kj::Maybe<InternalErrorId> internalErrorId;
  bool isDisconnection;
  bool isDoNotLogException;
};

struct DecodedException: public DecodedExceptionInfo {
  v8::Local<v8::Value> handle;
};

DecodedExceptionInfo initDecodedExceptionInfo(const TunneledErrorType& tunneledInfo) {
  return {
    .isInternal = tunneledInfo.isInternal,
    .isFromRemote = tunneledInfo.isFromRemote,
    .isDurableObjectReset = tunneledInfo.isDurableObjectReset,
    .internalErrorId = kj::none,
    .isDisconnection = false,
    .isDoNotLogException = tunneledInfo.isDoNotLogException,
  };
}

// Returns the id in `internalErrorId`, after storing a new one there if it has none.
InternalErrorId& ensureInternalErrorId(kj::Maybe<InternalErrorId>& internalErrorId) {
  KJ_IF_SOME(id, internalErrorId) {
    return id;
  }
  return internalErrorId.emplace(makeInternalErrorId());
}

// The message of a tunneled error: the part of its description that follows the error type,
// `errorString`. An internal error's message is hidden behind the id in `internalErrorId`, which
// gets a new id if it has none.
kj::String tunneledErrorMessage(const TunneledErrorType& tunneledInfo,
    kj::StringPtr errorString,
    kj::Maybe<InternalErrorId>& internalErrorId) {
  if (tunneledInfo.isInternal) {
    return renderInternalError(ensureInternalErrorId(internalErrorId));
  } else {
    return kj::str(trimErrorMessage(errorString));
  }
}

// If `errorType` begins with a DOMException and its parenthesized error name, like
// "DOMException(SyntaxError): message", returns the name and advances `errorType` past the closing
// parenthesis. A "DOMException(" that is never closed is skipped: `errorType` is advanced past it
// and whatever follows is left to be matched as an error type.
kj::Maybe<kj::String> tryConsumeDomExceptionName(kj::StringPtr& errorType) {
  if (errorType.startsWith("DOMException(")) {
    errorType = errorType.slice(strlen("DOMException("));
    // Check for closing brace
    KJ_IF_SOME(closeParen, errorType.findFirst(')')) {
      auto errorName = kj::str(errorType.first(closeParen));
      errorType = errorType.slice(1 + closeParen);
      return kj::mv(errorName);
    }
  }
  return kj::none;
}

// One of the standard error types that a tunneled error's description can name.
struct JsErrorType {
  kj::StringPtr name;
  v8::Local<v8::Value> (*construct)(v8::Local<v8::String> message, v8::Local<v8::Value> options);
};

// Returns the first error type whose name `errorType` starts with.
kj::Maybe<const JsErrorType&> tryFindJsErrorType(kj::StringPtr errorType) {
  static const JsErrorType TYPES[] = {
#define V(error_name, error_type) {error_name##_kj, &v8::Exception::error_type},
    JS_ERROR_TYPES(V)
#undef V
  };
  for (auto& type: TYPES) {
    if (errorType.startsWith(type.name)) return type;
  }
  return kj::none;
}

// Logs an internal exception to stderr and returns what an InternalExceptionObserver gets to know
// about it, after telling `observer` if there is one.
InternalExceptionObserver::Detail reportInternalException(const kj::Exception& exception,
    const DecodedExceptionInfo& info,
    kj::Maybe<const InternalExceptionObserver&> observer) {
  // Don't log exceptions that have been explicitly marked with worker_do_not_log or are
  // DISCONNECTED exceptions as these are unlikely to represent bugs worth tracking.
  bool shouldLogWithInternalId = !info.isDisconnection && !info.isDoNotLogException;
  InternalExceptionObserver::Detail detail{
    .isInternal = info.isInternal,
    .isFromRemote = info.isFromRemote,
    .isDurableObjectReset = info.isDurableObjectReset,
    .internalErrorId = shouldLogWithInternalId ? info.internalErrorId : kj::none,
  };
  KJ_IF_SOME(o, observer) {
    o.reportInternalException(exception, detail);
  }
  if (shouldLogWithInternalId) {
    auto& wdErrId = KJ_ASSERT_NONNULL(info.internalErrorId);
    LOG_EXCEPTION_WITH_ID("jsgInternalError", exception, wdErrId);
  } else {
    KJ_LOG(INFO, exception);  // Run with --verbose to see exception logs.
  }
  return detail;
}

DecodedException decodeTunneledException(
    v8::Isolate* isolate, const kj::Exception& exception, const ExceptionToJsOptions& options) {

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
  // https://heycam.github.io/webidl/#idl-exceptions
  //
  // TODO(someday): Support arbitrary user-defined error types, not just Error?
  auto tunneledInfo = tunneledErrorType(exception.getDescription());
  DecodedException result{initDecodedExceptionInfo(tunneledInfo)};
  KJ_IF_SOME(description, options.alreadyDescribed) {
    result.internalErrorId = description.internalErrorId;
  }

  auto errorType = tunneledInfo.message;
  auto appMessage = [&](kj::StringPtr errorString) -> kj::String {
    return tunneledErrorMessage(tunneledInfo, errorString, result.internalErrorId);
  };

  auto addAdditionalInfo = [isolate, &result, &exception]() {
    if (!result.handle->IsObject()) return;
    // Note that if the error was deserialized from the TUNNELED_EXCEPTION_DETAIL_ID detail,
    // these operations may overwrite properties that were already set on the serialized
    // error object. That is fine, we want the metadata captured in the kj::Exception
    // description to take precedence.
    // TODO(someday): Maybe consider making this configurable when a deserialized
    // error is used?
    if (result.isFromRemote) {
      setRemoteError(isolate, result.handle);
    }

    // A transient OVERLOADED failure gets both properties. Converting the error back to a
    // kj::Exception keeps OVERLOADED and drops the transient marker, so it is not retried after
    // passing through JavaScript, which avoids adding load to an overloaded service.
    if (isRetryable(exception)) {
      setRetryableError(isolate, result.handle);
    }
    if (exception.getType() == kj::Exception::Type::OVERLOADED) {
      setOverloadedError(isolate, result.handle);
    }

    if (result.isDurableObjectReset) {
      setDurableObjectResetError(isolate, result.handle);
    }

    KJ_IF_SOME(durableObjectId, getDurableObjectId(exception)) {
      setDurableObjectIdError(isolate, result.handle, durableObjectId);
    }
  };

  if (tunneledInfo.isJsgError) {
    // DOMExceptions require a parenthesized error name argument, like DOMException(SyntaxError).
    // TODO(someday): We always handle DOMException specifically here rather than decoding from
    // the serialized detail because using the detail breaks some tests that expect a specific
    // error details. We'll need to investigate further to see if we can make this more consistent.
    KJ_IF_SOME(errorName, tryConsumeDomExceptionName(errorType)) {
      auto& js = Lock::from(isolate);
      auto message = appMessage(errorType);
      auto exception = js.domException(kj::mv(errorName), kj::mv(message));
      result.handle = KJ_ASSERT_NONNULL(exception.tryGetHandle(js));
      addAdditionalInfo();
      return result;
    }

    // A payload written without enhanced error serialization uses V8's native error encoding,
    // which restores each error's original stack whatever the deserializer options say. Only a
    // trusted reader may use such a payload.
    auto& isolateBase = IsolateBase::from(isolate);
    bool canDropStacks = isolateBase.getUsingEnhancedErrorSerialization() &&
        exception.getDetail(TUNNELED_EXCEPTION_ENHANCED_DETAIL_ID) != kj::none;
    if ((options.trusted || canDropStacks) && !options.ignoreDetail) {
      // If the error was originally converted from a JS error, then we likely have
      // serialized the original error object as a detail, if so, let's try to use
      // that, otherwise, we'll fall back to constructing a new error object. If
      // the ignoreDetail optiom is set, we skip trying to deserialize.
      KJ_IF_SOME(serializedJsError, exception.getDetail(jsg::TUNNELED_EXCEPTION_DETAIL_ID)) {
        // NOLINTNEXTLINE(custom-raw-v8-trycatch): only logs what deserializing threw.
        v8::TryCatch tryCatch(isolate);
        try {
          auto& js = Lock::from(isolate);
          jsg::Deserializer deser(js, serializedJsError, kj::none, kj::none,
              jsg::Deserializer::Options{
                // By default, we do not preserve stacks in deserialized errors because
                // of concerns sharing stack details over potentially untrusted boundaries.
                // However, if the caller has explicitly indicated that the scope it trusted,
                // we will preserve the stack in the deserialized error.
                .preserveStackInErrors = options.trusted,
              });
          result.handle = deser.readValue(js);

          // If the result came from a serialized JS detail, it might not be an object!
          // If that's the case, and allowNonObjects is false (the default), we will ignore
          // the deserialized data and fallback to the normal decoding below.
          if (result.handle->IsObject() || options.allowNonObjects) {
            addAdditionalInfo();
            return result;
          }
        } catch (jsg::JsExceptionThrown&) {
          if (!tryCatch.CanContinue()) {
            tryCatch.ReThrow();
            throw;
          }
          // Failed to deserialize, we'll ignore the error and continue with the original
          // decoding below. For debugging purposes, when verbose logging is enabled, we
          // will at least log the error.
          if (!tryCatch.Exception().IsEmpty()) {
            KJ_LOG(INFO, "Failed to deserialize tunneled JS error detail", tryCatch.Exception());
          } else {
            KJ_LOG(INFO, "Failed to deserialize tunneled JS error detail (unknown error)");
          }
        }
      }
    }

    // It's neither a DOMException nor are we using a serialized detail, so it must be
    // one of the standard JS error types (or we will treat it as such).
    KJ_IF_SOME(type, tryFindJsErrorType(errorType)) {
      auto message = appMessage(errorType.slice(type.name.size()));
      result.handle = type.construct(v8Str(isolate, message), {});
      addAdditionalInfo();
      return result;
    }
  }

  // It's not a tunneled JavaScript error type that we recognize.
  // Return an internal error.
  result.isInternal = true;

  // For disconnection errors, we ignore any tunneled error info and just return
  // a generic "Network connection lost" error. One thing to keep in mind is that
  // DOMExceptions with the AbortError name are also DISCONNECTED errors, but those
  // are handled above as tunneled JS errors. It is important that we preserve the
  // ordering of these checks, so keep this if block after the tunneled JS error
  // handling above.
  if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
    result.isDisconnection = true;
    result.handle = v8::Exception::Error(v8StrIntern(isolate, NETWORK_CONNECTION_LOST));
    if (tunneledInfo.isFromRemote) {
      setRemoteError(isolate, result.handle);
    }

    // DISCONNECTED exceptions are considered retryable
    setRetryableError(isolate, result.handle);

    if (tunneledInfo.isDurableObjectReset) {
      setDurableObjectResetError(isolate, result.handle);
    }

    KJ_IF_SOME(durableObjectId, getDurableObjectId(exception)) {
      setDurableObjectIdError(isolate, result.handle, durableObjectId);
    }
  } else {
    // For everything return a generic error with an internal error id.
    result.handle = v8::Exception::Error(
        v8Str(isolate, renderInternalError(ensureInternalErrorId(result.internalErrorId))));
    addAdditionalInfo();
  }
  return result;
}

}  // namespace

kj::StringPtr extractTunneledExceptionDescription(kj::StringPtr message) {
  auto tunneledError = tunneledErrorType(message);
  if (tunneledError.isInternal) {
    // TODO(soon): Include an internal error ID in message, and also return the id.
    return "Error: internal error";
  } else {
    return tunneledError.message;
  }
}

v8::Local<v8::Value> exceptionToJs(
    v8::Isolate* isolate, kj::Exception&& exception, ExceptionToJsOptions options) {
  // TODO(cleanup): decodeTunneledException is currently only used here, consider
  // inlining it back into this function.
  auto tunneledException = decodeTunneledException(isolate, exception, options);

  if (tunneledException.isInternal && options.alreadyDescribed == kj::none) {
    reportInternalException(exception, tunneledException, IsolateBase::from(isolate).getObserver());
  }

  return tunneledException.handle;
}

namespace {

// V8's own error serialization, which a serialized detail written without enhanced error
// serialization uses, keeps the type of these errors only. Any other error comes back as a plain
// Error, whatever its `name` was.
kj::StringPtr nameAfterNativeErrorSerialization(kj::StringPtr name) {
  for (auto preserved: {"EvalError"_kj, "RangeError"_kj, "ReferenceError"_kj, "SyntaxError"_kj,
         "TypeError"_kj, "URIError"_kj}) {
    if (name == preserved) return preserved;
  }
  return "Error"_kj;
}

// Whether `stack`, the `stack` of an error with the given `name` and `message`, is no more than
// the "name: message" that a stack starts with, or the beginning of it. That is all that is left
// of a stack whose name and message fill the 4KB kept of it.
bool isStackWithoutFrames(kj::StringPtr stack, kj::StringPtr name, kj::StringPtr message) {
  if (stack.startsWith(name)) {
    stack = stack.slice(name.size());
    if (stack.startsWith(": "_kj)) {
      stack = stack.slice(2);
    }
  }
  return message.startsWith(stack);
}

// The properties of the object that exceptionToJs() deserializes from the serialized copy of a
// tunneled error, as far as the error's JsExceptionMetadata has them. A property that is missing
// or not a string is none.
struct TunneledErrorProperties {
  kj::Maybe<kj::String> name;
  kj::Maybe<kj::String> message;
  kj::Maybe<kj::String> stack;
};

// Reads the JsExceptionMetadata of an exception that also has a serialized copy of the error.
// exceptionToJs() rebuilds the error from that copy, so without the copy it does not see what the
// metadata holds either.
kj::Maybe<TunneledErrorProperties> tryReadTunneledErrorProperties(const kj::Exception& exception) {
  if (exception.getDetail(TUNNELED_EXCEPTION_DETAIL_ID) == kj::none) return kj::none;
  auto bytes = KJ_UNWRAP_OR_RETURN(exception.getDetail(JS_EXCEPTION_METADATA_DETAIL_ID), kj::none);
  if (bytes.size() % sizeof(capnp::word) != 0) return kj::none;

  // The detail is not necessarily aligned.
  auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(bytes);

  kj::Maybe<TunneledErrorProperties> result;
  // A detail can come from another process, so it may not be a valid message.
  KJ_IF_SOME(e, kj::runCatchingExceptions([&]() {
    capnp::FlatArrayMessageReader reader(words);
    auto metadata = reader.getRoot<JsExceptionMetadata>();
    auto& properties = result.emplace();
    if (metadata.getIsNativeError() &&
        exception.getDetail(TUNNELED_EXCEPTION_ENHANCED_DETAIL_ID) == kj::none) {
      properties.name = kj::str(nameAfterNativeErrorSerialization(metadata.getErrorType()));
    } else if (metadata.hasErrorType()) {
      properties.name = kj::str(metadata.getErrorType());
    }
    if (metadata.hasMessage()) {
      properties.message = kj::str(metadata.getMessage());
    }
    // The stack names the error as it was thrown, whatever becomes of its name when it is
    // serialized.
    kj::StringPtr thrownName = metadata.hasErrorType() ? metadata.getErrorType() : "Error"_kj;
    if (metadata.hasStackTrace() &&
        !isStackWithoutFrames(metadata.getStackTrace(), thrownName, metadata.getMessage())) {
      properties.stack = kj::str(metadata.getStackTrace());
    }
  })) {
    KJ_LOG(INFO, "Failed to read tunneled JS error metadata", e);
    return kj::none;
  }
  return result;
}

}  // namespace

JsErrorDescription describeAsJsError(
    const kj::Exception& exception, kj::Maybe<const InternalExceptionObserver&> observer) {
  // This mirrors decodeTunneledException() for a trusted reader, case by case.
  auto tunneledInfo = tunneledErrorType(exception.getDescription());
  auto info = initDecodedExceptionInfo(tunneledInfo);
  JsErrorDescription result;

  [&]() {
    auto errorType = tunneledInfo.message;

    if (tunneledInfo.isJsgError) {
      // A DOMException is rebuilt from the description, without the stack of the original.
      KJ_IF_SOME(errorName, tryConsumeDomExceptionName(errorType)) {
        result.name = kj::mv(errorName);
        result.message = tunneledErrorMessage(tunneledInfo, errorType, info.internalErrorId);
        return;
      }

      // Any other error is the original object, deserialized, if a copy of it came along.
      auto original = tryReadTunneledErrorProperties(exception);
      KJ_IF_SOME(properties, original) {
        KJ_IF_SOME(message, properties.message) {
          result.name = kj::mv(properties.name).orDefault([]() { return kj::str("Error"); });
          result.message = kj::mv(message);
          result.stack = kj::mv(properties.stack);
          return;
        }
      }

      // Otherwise it is a new error of the type that the description names, which has no stack
      // trace.
      KJ_IF_SOME(type, tryFindJsErrorType(errorType)) {
        result.name = kj::str(type.name);
        result.message = tunneledErrorMessage(
            tunneledInfo, errorType.slice(type.name.size()), info.internalErrorId);
        KJ_IF_SOME(properties, original) {
          // The original has no message that is a string, or its metadata predates the message
          // field. What the description says, which is how the original was stringified when it
          // was tunneled, stands in for the message.
          KJ_IF_SOME(name, properties.name) {
            result.name = kj::mv(name);
          }
          result.stack = kj::mv(properties.stack);
        }
        return;
      }
    }

    // It's not a tunneled JavaScript error type that we recognize.
    info.isInternal = true;
    result.name = kj::str("Error");
    if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
      info.isDisconnection = true;
      result.message = kj::str(NETWORK_CONNECTION_LOST);
    } else {
      result.message = renderInternalError(ensureInternalErrorId(info.internalErrorId));
    }
  }();

  result.internalErrorId = info.internalErrorId;
  if (info.isInternal) {
    result.internalException = reportInternalException(exception, info, observer);
  }
  return result;
}

Value Lock::exceptionToJs(kj::Exception&& exception, ExceptionToJsOptions options) {
  return withinHandleScope(
      [&] { return Value(v8Isolate, jsg::exceptionToJs(v8Isolate, kj::mv(exception), options)); });
}

JsRef<JsValue> Lock::exceptionToJsValue(kj::Exception&& exception, ExceptionToJsOptions options) {
  return withinHandleScope([&] {
    JsValue val = JsValue(jsg::exceptionToJs(v8Isolate, kj::mv(exception), options));
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

void throwInternalError(
    v8::Isolate* isolate, kj::Exception&& exception, ExceptionToJsOptions options) {
  KJ_IF_SOME(renderingError, kj::runCatchingExceptions([&]() {
    isolate->ThrowException(exceptionToJs(isolate, kj::mv(exception), options));
  })) {
    KJ_LOG(ERROR, "error rendering exception", renderingError);
    KJ_LOG(ERROR, exception);
    throwInternalError(isolate, "error rendering exception");
  }
}

bool startsWithTunneledErrorType(kj::ArrayPtr<const char> description) {
  static const constexpr kj::StringPtr NAMES[] = {
#define V(name, _) name##_kj,
    JS_ERROR_TYPES(V)
#undef V
        "DOMException"_kj,
  };
  for (auto name: NAMES) {
    if (description.startsWith(name.asArray())) return true;
  }
  return false;
}

void addExceptionDetail(Lock& js, kj::Exception& exception, v8::Local<v8::Value> handle) {
  // NOLINTNEXTLINE(custom-raw-v8-trycatch): only rethrows a termination.
  v8::TryCatch tryCatch(js.v8Isolate);
  try {
    Serializer ser(js, {.version = TUNNELED_EXCEPTION_SERIALIZATION_VERSION});
    ser.write(js, JsValue(handle));
    exception.setDetail(TUNNELED_EXCEPTION_DETAIL_ID, ser.release().data);
    if (js.isUsingEnhancedErrorSerialization()) {
      exception.setDetail(TUNNELED_EXCEPTION_ENHANCED_DETAIL_ID, kj::heapArray<kj::byte>({1}));
    } else {
      exception.releaseDetail(TUNNELED_EXCEPTION_ENHANCED_DETAIL_ID);
    }
  } catch (JsExceptionThrown&) {
    if (tryCatch.HasTerminated()) {
      // Execution is being terminated. Let it continue to unwind: swallowing it here would let
      // the caller carry on running as if execution could continue. The TryCatch's destructor
      // hands the termination back to V8 if JavaScript is on the stack.
      throw;
    }
    // The exception is not serializable. Proceed without annotating.
  }
}

void addDurableObjectId(kj::Exception& exception, kj::StringPtr durableObjectId) {
  exception.setDetail(
      DURABLE_OBJECT_EXCEPTION_METADATA_DETAIL_ID, kj::heapArray(durableObjectId.asBytes()));
}

kj::Maybe<kj::String> getDurableObjectId(const kj::Exception& exception) {
  KJ_IF_SOME(detail, exception.getDetail(DURABLE_OBJECT_EXCEPTION_METADATA_DETAIL_ID)) {
    return kj::str(detail.asChars());
  }
  return kj::none;
}

namespace {

// Returns the first `size` bytes of `text`, or fewer so as not to end in the middle of a UTF-8
// sequence. `size` must be less than the size of `text`.
kj::String truncateUtf8(kj::StringPtr text, size_t size) {
  while (size > 0 && (static_cast<kj::byte>(text[size]) & 0xc0) == 0x80) {
    --size;
  }
  return kj::str(text.first(size));
}

}  // namespace

void addJsExceptionMetadata(Lock& js, kj::Exception& exception, v8::Local<v8::Value> handle) {
  // Extract JavaScript error type and stack trace
  if (!handle->IsObject()) {
    return;  // Not an error object, nothing to extract
  }

  auto errorObj = jsg::JsObject(handle.As<v8::Object>());

  // Build Cap'n Proto message
  capnp::MallocMessageBuilder message;
  auto metadata = message.initRoot<JsExceptionMetadata>();

  // Extract error name (e.g., "Error", "TypeError", "RangeError")
  auto nameProp = errorObj.get(js, "name"_kj);
  if (nameProp.isString()) {
    auto errorType = nameProp.toString(js);
    // Truncate to 4KB if needed
    if (errorType.size() > JS_EXCEPTION_METADATA_MAX_FIELD_SIZE) {
      errorType = truncateUtf8(errorType, JS_EXCEPTION_METADATA_MAX_FIELD_SIZE);
    }
    metadata.setErrorType(errorType);
  }

  // Extract stack trace string
  auto stackProp = errorObj.get(js, "stack"_kj);
  if (stackProp.isString()) {
    auto stackTrace = stackProp.toString(js);
    // Truncate to 4KB if needed
    if (stackTrace.size() > JS_EXCEPTION_METADATA_MAX_FIELD_SIZE) {
      stackTrace = truncateUtf8(stackTrace, JS_EXCEPTION_METADATA_MAX_FIELD_SIZE);
    }
    metadata.setStackTrace(stackTrace);
  }

  // Extract error message
  auto messageProp = errorObj.get(js, "message"_kj);
  if (messageProp.isString()) {
    auto errorMessage = messageProp.toString(js);
    // Truncate to 4KB if needed
    if (errorMessage.size() > JS_EXCEPTION_METADATA_MAX_FIELD_SIZE) {
      errorMessage = truncateUtf8(errorMessage, JS_EXCEPTION_METADATA_MAX_FIELD_SIZE);
    }
    metadata.setMessage(errorMessage);
  }

  metadata.setIsNativeError(handle->IsNativeError());

  // Serialize to bytes using Cap'n Proto
  auto words = capnp::messageToFlatArray(message);
  exception.setDetail(JS_EXCEPTION_METADATA_DETAIL_ID, kj::heapArray(words.asBytes()));
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
  kj::throwException(createTunneledException(isolate, exception));
}

kj::Exception createTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(SET_DATA_ISOLATE_BASE));
  auto& js = Lock::from(isolate);
  return jsgIsolate.unwrapException(js, isolate->GetCurrentContext(), exception);
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

// The returned kj::Array<kj::byte> aliases the V8 BackingStore (kept alive via the attached
// shared_ptr).  Two consequences:
//
//   * When MPK protects isolate memory, the bytes live in V8 sandbox pages tagged with the
//     isolate's pkey.  Access is then only valid while the isolate lock is held.  Handing this
//     Array to async I/O that runs without the lock will fault.  Callers that cross a lock
//     boundary must make their own kj-heap copy (e.g. via `kj::heapArray(result.asPtr())`)
//     before the await.
//
//   * Writes through the Array land in the JS BackingStore.  This is the contract Pyodide's
//     ReadOnlyBuffer::read and similar "destination buffer" APIs rely on.  An always-copy
//     implementation would silently break those callers: their data would land in a temporary
//     that is dropped on return.
kj::Array<kj::byte> asBytes(v8::Local<v8::ArrayBuffer> arrayBuffer) {
  if (arrayBuffer->IsResizableByUserJavaScript() || arrayBuffer->IsImmutable()) {
    // For resizable ArrayBuffers, resize(0) decommits pages (PROT_NONE) even while the
    // BackingStore shared_ptr is held. Deep-copy to prevent SIGSEGV if JS shrinks the
    // buffer after we capture the pointer. We use arrayBuffer->ByteLength() (the live
    // length) rather than backing->ByteLength() (which returns the max reservation size).
    // Ref: AUTOVULN-CLOUDFLARE-WORKERD-73
    //
    // We also want to copy for immutable ArrayBuffers. Since the expectation might
    // be that the memory buffer returned from asBytes() is mutable, we don't want
    // to violate the expectation.
    auto byteLength = arrayBuffer->ByteLength();
    if (byteLength == 0) {
      return getEmptyArray();
    }
    kj::ArrayPtr bytes(static_cast<kj::byte*>(arrayBuffer->Data()), byteLength);
    return kj::heapArray<kj::byte>(bytes);
  }
  auto backing = arrayBuffer->GetBackingStore();
  kj::ArrayPtr bytes(static_cast<kj::byte*>(backing->Data()), backing->ByteLength());
  if (bytes == nullptr) {
    return getEmptyArray();
  }
  return bytes.attach(kj::mv(backing));
}

// See the ArrayBuffer overload above for the aliasing contract (MPK + write-through).
kj::Array<kj::byte> asBytes(v8::Local<v8::ArrayBufferView> arrayBufferView) {
  auto buffer = arrayBufferView->Buffer();
  if (buffer->IsResizableByUserJavaScript() || buffer->IsImmutable()) {
    // Deep-copy for resizable or immutable ArrayBuffers -- see comment above.
    // CopyContents handles bounds checking internally for out-of-bounds views.
    auto len = arrayBufferView->ByteLength();
    if (len == 0) {
      return getEmptyArray();
    }
    auto copy = kj::heapArray<kj::byte>(len);
    arrayBufferView->CopyContents(copy.begin(), copy.size());
    return copy;
  }
  auto backing = buffer->GetBackingStore();
  kj::ArrayPtr bufferBytes(static_cast<kj::byte*>(backing->Data()), backing->ByteLength());
  auto sliceStart = arrayBufferView->ByteOffset();
  auto sliceEnd = sliceStart + arrayBufferView->ByteLength();
  KJ_ASSERT(bufferBytes.size() >= sliceEnd);
  auto bytes = bufferBytes.slice(sliceStart, sliceEnd);
  if (bytes == nullptr) {
    return getEmptyArray();
  }
  return bytes.attach(kj::mv(backing));
}

// See the ArrayBuffer overload above for the aliasing contract (MPK + write-through).
kj::Array<kj::byte> asBytes(v8::Local<v8::SharedArrayBuffer> sharedArrayBuffer) {
  auto backing = sharedArrayBuffer->GetBackingStore();
  kj::ArrayPtr bytes(static_cast<kj::byte*>(backing->Data()), backing->ByteLength());
  if (bytes == nullptr) {
    return getEmptyArray();
  } else {
    return bytes.attach(kj::mv(backing));
  }
}

void recursivelyFreeze(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  if (value->IsArray()) {
    // Optimize array freezing (Array is a subclass of Object, but we can iterate it faster).
    v8::HandleScope scope(v8::Isolate::GetCurrent());
    auto arr = value.As<v8::Array>();

    for (auto i: kj::zeroTo(arr->Length())) {
      recursivelyFreeze(context, check(arr->Get(context, i)));
    }

    check(arr->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen));
  } else if (value->IsObject()) {
    v8::HandleScope scope(v8::Isolate::GetCurrent());
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
  returnRejectedPromiseImpl<const v8::FunctionCallbackInfo<v8::Value>&>(info, exception, tryCatch);
}

void returnRejectedPromise(const v8::PropertyCallbackInfo<v8::Value>& info,
    v8::Local<v8::Value> exception,
    v8::TryCatch& tryCatch) {
  returnRejectedPromiseImpl<const v8::PropertyCallbackInfo<v8::Value>&>(info, exception, tryCatch);
}

// Default allocator that uses standard new/delete.
class DefaultExternalStringAllocator final: public ExternalStringAllocator {
 public:
  void* allocate(size_t size) override {
    return operator new(size);
  }
  void deallocate(void* ptr) override {
    operator delete(ptr);
  }
};

kj::Own<ExternalStringAllocator> defaultExternalStringAllocator() {
  static DefaultExternalStringAllocator allocator;
  return kj::Own<ExternalStringAllocator>(&allocator, kj::NullDisposer::instance);
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
  using Backing = kj::OneOf<kj::ArrayPtr<const Data>, kj::Arc<kj::Array<const Data>>>;

  inline const Data* data() const override {
    return getBuffer().begin();
  }

  inline size_t length() const override {
    return getBuffer().size();
  }

  inline uint64_t byteLength() const {
    return length() * sizeof(Data);
  }

  static v8::MaybeLocal<v8::String> createExtern(v8::Isolate* isolate, Backing backing) {
    if (getBuffer(backing).size() == 0) {
      return v8::String::Empty(isolate);
    }

    // TODO(now): In Node.js impl, we check to see if length is less than a specified
    // minimum. If it is, it's likely more efficient to just copy and use a regular
    // heap allocated string than an external. We're not doing that here currently, but
    // we might?

    auto resource = new (std::nothrow) ExternString<Type, Data>(kj::mv(backing));
    if (resource == nullptr) {
      isolate->ThrowException(v8::Exception::Error(
          v8::String::NewFromUtf8Literal(isolate, "String allocation failed")));
      return v8::MaybeLocal<v8::String>();
    }

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
      isolate->ThrowException(v8::Exception::Error(
          v8::String::NewFromUtf8Literal(isolate, "String allocation failed")));
      return v8::MaybeLocal<v8::String>();
    }

    return str;
  }

 private:
  Backing backing;

  static kj::ArrayPtr<const Data> getBuffer(const Backing& backing) {
    if (backing.template is<kj::ArrayPtr<const Data>>()) {
      return backing.template get<kj::ArrayPtr<const Data>>();
    }
    return backing.template get<kj::Arc<kj::Array<const Data>>>()->asPtr();
  }

  kj::ArrayPtr<const Data> getBuffer() const {
    return getBuffer(backing);
  }

  explicit ExternString(Backing backing): backing(kj::mv(backing)) {}
};

using ExternOneByteString = ExternString<v8::String::ExternalOneByteStringResource, char>;
using ExternTwoByteString = ExternString<v8::String::ExternalStringResource, uint16_t>;

v8::Local<v8::String> newExternalOneByteString(Lock& js, kj::ArrayPtr<const char> buf) {
  return check(ExternOneByteString::createExtern(js.v8Isolate, buf));
}

v8::Local<v8::String> newExternalOneByteString(Lock& js, kj::Arc<OwnedAscii> buf) {
  return check(ExternOneByteString::createExtern(js.v8Isolate, kj::mv(buf)));
}

v8::Local<v8::String> newExternalTwoByteString(Lock& js, kj::ArrayPtr<const uint16_t> buf) {
  return check(ExternTwoByteString::createExtern(js.v8Isolate, buf));
}

v8::Local<v8::String> newExternalTwoByteString(Lock& js, kj::Arc<OwnedUtf16> buf) {
  return check(ExternTwoByteString::createExtern(js.v8Isolate, kj::mv(buf)));
}

v8::Local<v8::String> newExternalString(Lock& js, StaticExternalStringSource buf) {
  KJ_SWITCH_ONEOF(buf) {
    KJ_CASE_ONEOF(oneByte, kj::ArrayPtr<const char>) {
      return newExternalOneByteString(js, oneByte);
    }
    KJ_CASE_ONEOF(twoByte, kj::ArrayPtr<const uint16_t>) {
      return newExternalTwoByteString(js, twoByte);
    }
  }
  KJ_UNREACHABLE;
}

v8::Local<v8::String> newExternalString(Lock& js, const ExternalStringSource& buf) {
  KJ_SWITCH_ONEOF(buf) {
    KJ_CASE_ONEOF(oneByte, kj::ArrayPtr<const char>) {
      return newExternalOneByteString(js, oneByte);
    }
    KJ_CASE_ONEOF(twoByte, kj::ArrayPtr<const uint16_t>) {
      return newExternalTwoByteString(js, twoByte);
    }
    KJ_CASE_ONEOF(oneByte, kj::Arc<OwnedAscii>) {
      return newExternalOneByteString(js, oneByte.addRef());
    }
    KJ_CASE_ONEOF(twoByte, kj::Arc<OwnedUtf16>) {
      return newExternalTwoByteString(js, twoByte.addRef());
    }
  }
  KJ_UNREACHABLE;
}

// An external string resource over characters from the isolate's ExternalStringAllocator. The
// resource itself is on the process heap: V8 calls through its vtable, which must not be
// writable from the sandbox.
template <typename Type, typename Data>
class AllocatedExternString final: public Type {
 public:
  AllocatedExternString(ExternalStringAllocator& allocator, kj::ArrayPtr<Data> chars)
      : allocator(allocator),
        chars(chars) {}
  ~AllocatedExternString() noexcept(true) {
    allocator.deallocate(chars.begin());
  }
  KJ_DISALLOW_COPY_AND_MOVE(AllocatedExternString);

  const Data* data() const override {
    return chars.begin();
  }
  size_t length() const override {
    return chars.size();
  }

 private:
  ExternalStringAllocator& allocator;
  kj::ArrayPtr<Data> chars;
};

template <typename Type, typename Data>
v8::Local<v8::String> copyToExternalString(Lock& js, kj::ArrayPtr<const Data> buf) {
  if (buf.size() == 0) return v8::String::Empty(js.v8Isolate);

  auto& allocator = IsolateBase::from(js.v8Isolate).getExternalStringAllocator();
  auto* chars = static_cast<Data*>(allocator.allocate(buf.size() * sizeof(Data)));
  JSG_REQUIRE(chars != nullptr, RangeError, "String allocation failed");
  memcpy(chars, buf.begin(), buf.size() * sizeof(Data));
  // We are passing this to V8, which will `delete` it when the external string is garbage
  // collected, so we use a plain `new` here.
  auto* resource =
      new AllocatedExternString<Type, Data>(allocator, kj::arrayPtr(chars, buf.size()));

  v8::MaybeLocal<v8::String> str;
  if constexpr (kj::isSameType<Type, v8::String::ExternalOneByteStringResource>()) {
    str = v8::String::NewExternalOneByte(js.v8Isolate, resource);
  } else {
    str = v8::String::NewExternalTwoByte(js.v8Isolate, resource);
  }
  if (str.IsEmpty()) {
    // Only a string over V8's maximum length fails.
    delete resource;
    JSG_FAIL_REQUIRE(RangeError, "String allocation failed");
  }
  return str.ToLocalChecked();
}

v8::Local<v8::String> copyToExternalOneByteString(Lock& js, kj::ArrayPtr<const char> buf) {
  return copyToExternalString<v8::String::ExternalOneByteStringResource>(js, buf);
}

v8::Local<v8::String> copyToExternalTwoByteString(Lock& js, kj::ArrayPtr<const uint16_t> buf) {
  return copyToExternalString<v8::String::ExternalStringResource>(js, buf);
}

// ======================================================================================
// Module utilities

JsObject createMutableModuleExports(Lock& js, JsObject moduleNamespace) {
  auto result = js.objNoProto();
  auto names = moduleNamespace.getPropertyNames(js, OWN_ONLY, ALL_PROPERTIES, INCLUDE_INDICES);

  for (uint32_t i = 0; i < names.size(); i++) {
    auto name = names.get(js, i);
    result.set(js, name, moduleNamespace.get(js, name));
  }

  return result;
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
  "readline"_kj, "readline/promises"_kj, "repl"_kj, "sqlite"_kj, "stream"_kj, "stream/consumers"_kj,
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

bool isNodeJsProcessV2Enabled(jsg::Lock& js) {
  return IsolateBase::from(js.v8Isolate).isNodeJsProcessV2Enabled();
}

bool isRequireReturnsDefaultExportEnabled(jsg::Lock& js) {
  return IsolateBase::from(js.v8Isolate).isRequireReturnsDefaultExportEnabled();
}

}  // namespace workerd::jsg
