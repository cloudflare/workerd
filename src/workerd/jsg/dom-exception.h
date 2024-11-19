// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jsg.h"
#include "ser.h"

#define JSG_DOM_EXCEPTION_FOR_EACH_ERROR_NAME(f)                                                   \
  f(INDEX_SIZE_ERR, 1, "IndexSizeError") f(DOMSTRING_SIZE_ERR, 2, "DOMStringSizeError")            \
      f(HIERARCHY_REQUEST_ERR, 3, "HierarchyRequestError") f(WRONG_DOCUMENT_ERR, 4,                \
          "WrongDocumentError") f(INVALID_CHARACTER_ERR, 5, "InvalidCharacterError")               \
          f(NO_DATA_ALLOWED_ERR, 6, "NoDataAllowedError")                                          \
              f(NO_MODIFICATION_ALLOWED_ERR, 7, "NoModificationAllowedError") f(                   \
                  NOT_FOUND_ERR, 8, "NotFoundError") f(NOT_SUPPORTED_ERR, 9, "NotSupportedError")  \
                  f(INUSE_ATTRIBUTE_ERR, 10, "InUseAttributeError") f(                             \
                      INVALID_STATE_ERR, 11, "InvalidStateError") f(SYNTAX_ERR, 12, "SyntaxError") \
                      f(INVALID_MODIFICATION_ERR, 13, "InvalidModificationError") f(NAMESPACE_ERR, \
                          14, "NamespaceError") f(INVALID_ACCESS_ERR, 15, "InvalidAccessError")    \
                          f(VALIDATION_ERR, 16, "ValidationError") f(TYPE_MISMATCH_ERR, 17,        \
                              "TypeMismatchError") f(SECURITY_ERR, 18, "SecurityError")            \
                              f(NETWORK_ERR, 19, "NetworkError") f(ABORT_ERR, 20, "AbortError")    \
                                  f(URL_MISMATCH_ERR, 21, "URLMismatchError")                      \
                                      f(QUOTA_EXCEEDED_ERR, 22, "QuotaExceededError")              \
                                          f(TIMEOUT_ERR, 23, "TimeoutError")                       \
                                              f(INVALID_NODE_TYPE_ERR, 24, "InvalidNodeTypeError") \
                                                  f(DATA_CLONE_ERR, 25, "DataCloneError")

namespace workerd::jsg {

// JSG allows DOMExceptions to be tunneled through kj::Exceptions (see makeInternalError() for
// details). While this feature is activated conditionally at run-time, and thus does not depend
// on any specific concrete C++ type, JSG needs to be able to unit test the tunneled exception
// functionality, thus the existence of this implementation.
//
// Note that DOMException is currently the only user-defined exception to get this special
// treatment because it is the only non-builtin JS exception that standard web APIs are allowed to
// throw, per Web IDL.
//
// Users of JSG are free (and encouraged) to use this implementation, but they can also opt into
// the same tunneled exception feature by defining their own globally-accessible type named
// "DOMException".
class DOMException: public Object {
 public:
  DOMException(kj::String message, kj::String name): message(kj::mv(message)), name(kj::mv(name)) {}

  // JS API

  static Ref<DOMException> constructor(const v8::FunctionCallbackInfo<v8::Value>& args,
      Optional<kj::String> message,
      Optional<kj::String> name);

  kj::StringPtr getName() const;
  kj::StringPtr getMessage() const;
  int getCode() const;

#define JSG_DOM_EXCEPTION_CONSTANT_CXX(name, code, friendlyName) static constexpr int name = code;
#define JSG_DOM_EXCEPTION_CONSTANT_JS(name, code, friendlyName) JSG_STATIC_CONSTANT(name);

  // Define constexpr codes for every INDEX_SIZE_ERR, DOMSTRING_SIZE_ERR, etc.
  JSG_DOM_EXCEPTION_FOR_EACH_ERROR_NAME(JSG_DOM_EXCEPTION_CONSTANT_CXX)

  JSG_RESOURCE_TYPE(DOMException) {
    JSG_INHERIT_INTRINSIC(v8::kErrorPrototype);

    // TODO(conform): Per the spec, these should be prototype properties
    // and not instance properties. Fixing this does require use of the
    // flags.getJsgPropertyOnPrototypeTemplate() compatibility flag.
    // The standard definition of DOMException can be found here:
    // https://webidl.spec.whatwg.org/#idl-DOMException
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(message, getMessage);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(name, getName);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(code, getCode);

    // Declare static JS constants for every INDEX_SIZE_ERR, DOMSTRING_SIZE_ERR, etc.
    JSG_DOM_EXCEPTION_FOR_EACH_ERROR_NAME(JSG_DOM_EXCEPTION_CONSTANT_JS)

    JSG_TS_OVERRIDE({
      get stack(): any;
      set stack(value: any);
    });
  }
  JSG_REFLECTION(stack);

#undef JSG_DOM_EXCEPTION_CONSTANT_CXX
#undef JSG_DOM_EXCEPTION_CONSTANT_JS

  void visitForMemoryInfo(MemoryTracker& tracker) const {
    tracker.trackField("message", message);
    tracker.trackField("name", name);
  }

  // TODO(cleanup): The value is taken from worker-interface.capnp, which we can't
  // depend on directly here because we cannot introduce the dependency into JSG.
  // Therefore we have to set it manually. A better solution long term is to actually
  // move DOMException into workerd/api, but we'll do that separately.
  static constexpr uint SERIALIZATION_TAG = 7;
  static constexpr uint SERIALIZATION_TAG_V2 = 8;
  JSG_SERIALIZABLE(SERIALIZATION_TAG_V2, SERIALIZATION_TAG);

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<DOMException> deserialize(
      jsg::Lock& js, uint tag, jsg::Deserializer& deserializer);

 private:
  kj::String message;
  kj::String name;
  PropertyReflection<kj::String> stack;
};

}  // namespace workerd::jsg
