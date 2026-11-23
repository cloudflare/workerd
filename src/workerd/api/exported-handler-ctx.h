// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Temporary: this header exists only while the C++ and Rust implementations of ExecutionContext
// coexist behind the rust-execution-context autogate. Once the C++ api::ExecutionContext is
// removed, the runtime holds the Rust one directly and this header goes away.
//
// The `ctx` of an ExportedHandler, which is either implementation of ExecutionContext. A header
// of its own because worker.h holds one by value, and global-scope.h depends on worker.h.

#include <workerd/jsg/jsg.h>

namespace workerd::api {

class ExecutionContext;

namespace exportedhandler {

// The `ctx` the runtime passes to a handler: either the C++ api::ExecutionContext, or the Rust
// one from src/workerd/api/execution-context/, as Worker::Api::newExecutionContext() chooses.
// The Rust one is held by its JavaScript wrapper, which keeps the Rust resource alive. The class
// is named after the type users know, and lives in a namespace of its own, so that JSG's
// conversion errors name the type as before. RTTI describes it as the C++ class.
class ExecutionContext {
 public:
  using JsgRttiDelegate = jsg::Ref<api::ExecutionContext>;

  // Defined out of line, in global-scope.c++, because the C++ class is incomplete here.
  explicit ExecutionContext(jsg::Ref<api::ExecutionContext> ctx);
  ExecutionContext(ExecutionContext&& other);
  ExecutionContext& operator=(ExecutionContext&& other);
  ~ExecutionContext() noexcept(false);
  KJ_DISALLOW_COPY(ExecutionContext);

  // `wrapper` must be the JavaScript wrapper of a Rust ExecutionContext.
  static ExecutionContext fromRust(jsg::Lock& js, jsg::JsObject wrapper);

  ExecutionContext addRef(jsg::Lock& js);

  // The JavaScript object.
  jsg::JsObject getHandle(jsg::Lock& js);

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      ExecutionContext value) {
    KJ_SWITCH_ONEOF(value.impl) {
      KJ_CASE_ONEOF(cpp, jsg::Ref<api::ExecutionContext>) {
        return typeWrapper.wrap(js, context, creator, kj::mv(cpp));
      }
      KJ_CASE_ONEOF(rust, jsg::JsRef<jsg::JsObject>) {
        return rust.getHandle(js);
      }
    }
    KJ_UNREACHABLE;
  }

  static kj::Maybe<ExecutionContext> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    KJ_IF_SOME(cpp,
        typeWrapper.tryUnwrap(js, context, handle,
            static_cast<jsg::Ref<api::ExecutionContext>*>(nullptr), parentObject)) {
      return ExecutionContext(kj::mv(cpp));
    }
    return tryUnwrapRust(js, handle);
  }

  void visitForGc(jsg::GcVisitor& visitor);

 private:
  explicit ExecutionContext(jsg::JsRef<jsg::JsObject> rust);

  // `handle` as a Rust ExecutionContext, if it is one; asks Worker::Api, so that only the
  // binaries that create Rust ExecutionContexts link the Rust crate.
  static kj::Maybe<ExecutionContext> tryUnwrapRust(jsg::Lock& js, v8::Local<v8::Value> handle);

  kj::OneOf<jsg::Ref<api::ExecutionContext>, jsg::JsRef<jsg::JsObject>> impl;
};

}  // namespace exportedhandler

}  // namespace workerd::api
