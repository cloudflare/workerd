// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The `ctx` of an ExportedHandler. A header of its own because worker.h holds one by value, and
// global-scope.h depends on worker.h.

#include <workerd/jsg/jsg.h>

namespace workerd::api::exportedhandler {

// The `ctx` the runtime passes to a handler: the Rust ExecutionContext from
// src/workerd/api/execution-context/, as Worker::Api::newExecutionContext() creates it. It is
// held by its JavaScript wrapper, which keeps the Rust resource alive. The class is named after
// the type users know, and lives in a namespace of its own, so that JSG's conversion errors name
// that type.
class ExecutionContext {
 public:
  // RTTI describes the type as the Rust resource, whose std::any::type_name() is IDENTITY.
  struct JsgRttiRustStructure {
    static constexpr kj::StringPtr NAME = "ExecutionContext"_kj;
    static constexpr kj::StringPtr IDENTITY = "execution_context::ExecutionContext"_kj;
  };

  // `wrapper` must be the JavaScript wrapper of a Rust ExecutionContext.
  ExecutionContext(jsg::Lock& js, jsg::JsObject wrapper): wrapper(js, wrapper) {}
  ExecutionContext(ExecutionContext&&) = default;
  ExecutionContext& operator=(ExecutionContext&&) = default;
  KJ_DISALLOW_COPY(ExecutionContext);

  ExecutionContext addRef(jsg::Lock& js) {
    return ExecutionContext(wrapper.addRef(js));
  }

  // The JavaScript object.
  jsg::JsObject getHandle(jsg::Lock& js) {
    return wrapper.getHandle(js);
  }

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      ExecutionContext value) {
    return value.getHandle(js);
  }

  static kj::Maybe<ExecutionContext> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return tryUnwrap(js, handle);
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(wrapper);
  }

 private:
  explicit ExecutionContext(jsg::JsRef<jsg::JsObject> wrapper): wrapper(kj::mv(wrapper)) {}

  // `handle` as an ExecutionContext, if it is one. Defined in global-scope.c++: it asks
  // Worker::Api, so that only the binaries that create ExecutionContexts link the Rust crate.
  static kj::Maybe<ExecutionContext> tryUnwrap(jsg::Lock& js, v8::Local<v8::Value> handle);

  jsg::JsRef<jsg::JsObject> wrapper;
};

}  // namespace workerd::api::exportedhandler
