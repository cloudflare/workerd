// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/v8-platform-wrapper.h>
#include <workerd/rust/jsg/ffi.h>

#include <rust/cxx.h>
#include <v8-wasm.h>

#include <kj/async.h>
#include <kj/refcount.h>

#include <memory>

namespace workerd::api::wasm {

struct ModuleBytes;

// One streaming compilation: V8's handle, which must get exactly one Finish() or Abort(). If it is
// destroyed without either, it aborts from the isolate's next foreground task, since it may be
// destroyed without the isolate lock.
class WasmStreaming {
 public:
  WasmStreaming(v8::Isolate* isolate, std::shared_ptr<v8::WasmStreaming> streaming);
  ~WasmStreaming() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(WasmStreaming);

  void onBytesReceived(::rust::Slice<const uint8_t> bytes);
  void setUrl(::rust::Str url);
  void finish();
  void abort(::workerd::rust::jsg::Local exception);

 private:
  v8::Isolate* isolate;
  std::shared_ptr<v8::WasmStreaming> streaming;
  std::shared_ptr<v8::TaskRunner> foregroundTaskRunner;
  bool done = false;
};

// The signal that the isolate's foreground task runners notify whenever V8 posts a task.
using ForegroundTaskSignal = const jsg::ForegroundTaskSignal;

kj::Arc<ForegroundTaskSignal> foreground_task_signal(::workerd::rust::jsg::Isolate* isolate);
uint64_t foreground_task_signal_epoch(ForegroundTaskSignal& signal);
void foreground_task_signal_notify(ForegroundTaskSignal& signal);
kj::Promise<void> foreground_task_signal_wait_after(ForegroundTaskSignal& signal, uint64_t seen);

// A module's bytes on their way to the embedder: the promise that
// IoChannelFactory::submitWasmModule() returned for them.
struct WasmSubmission {
  kj::Promise<void> accepted;
};

// Hands `bytes` to the current request's IoChannelFactory::submitWasmModule(), which may refuse
// them by throwing. The caller holds the isolate lock in a request.
kj::Own<WasmSubmission> submit_wasm_module(::rust::Box<ModuleBytes> bytes);
// Resolves once the embedder accepts the module, and fails if it rejects it.
kj::Promise<void> wasm_submission_wait(kj::Own<WasmSubmission> submission);

// Condemns the isolate through the current request's IoChannelFactory::abortIsolate(), because the
// embedder refused a module the Worker already has.
void abort_isolate_for_refused_module();

// Sets the isolate's WebAssembly streaming and module callbacks and its code generation hook to
// the crate's.
void install_wasm_callbacks(::workerd::rust::jsg::Isolate* isolate);

}  // namespace workerd::api::wasm
