// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/string.h>

// Forward declare v8::Isolate here, this allows us to avoid including the V8 header and compile
// some targets without depending on V8.
namespace v8 {
  class Isolate;
}

namespace workerd::jsg {

struct CompilationObserver {
  virtual ~CompilationObserver() noexcept(false) { }

  // see ModuleInfoCompileOption
  enum class Option { BUNDLE, BUILTIN };

  // Monitors behavior of compilation processes.

  // Called at the start of module compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during both invocations.
  virtual kj::Own<void> onEsmCompilationStart(
      v8::Isolate* isolate, kj::StringPtr name, Option option) const { return kj::Own<void>(); }

  // Called at the start of wasm compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during both invocations.
  virtual kj::Own<void> onWasmCompilationStart(v8::Isolate* isolate, size_t codeSize) const {
    return kj::Own<void>();
  }
};


struct IsolateObserver : public CompilationObserver {
  virtual ~IsolateObserver() noexcept(false) { }
};


} // namespace workerd::jsg
