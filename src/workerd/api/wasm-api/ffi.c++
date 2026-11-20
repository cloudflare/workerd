// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/api/wasm-api/ffi.rs.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/setup.h>
#include <workerd/rust/jsg/ffi-inl.h>

#include <v8-platform.h>

namespace workerd::api::wasm {

namespace {

using ::workerd::rust::jsg::local_from_ffi;
using ::workerd::rust::jsg::to_ffi;

// Aborts a streaming compilation that nothing will finish. Runs as a foreground task, so with the
// isolate lock.
class AbortWasmStreamingTask final: public v8::Task {
 public:
  AbortWasmStreamingTask(v8::Isolate* isolate, std::shared_ptr<v8::WasmStreaming> streaming)
      : isolate(isolate),
        streaming(kj::mv(streaming)) {}

  void Run() override {
    v8::HandleScope scope(isolate);
    // Without an exception, V8 drops the compilation and leaves its promise pending.
    streaming->Abort({});
  }

 private:
  v8::Isolate* isolate;
  std::shared_ptr<v8::WasmStreaming> streaming;
};

void wasmStreamingCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto isolate = info.GetIsolate();
  auto streaming =
      kj::heap<WasmStreaming>(isolate, v8::WasmStreaming::Unpack(isolate, info.DataV2()));
  wasm_streaming_callback(isolate, to_ffi(info[0]), kj::mv(streaming));
}

bool wasmModuleCallback(v8::Isolate* isolate, v8::Local<v8::Value> source) {
  v8::HandleScope scope(isolate);
  return wasm_module_callback(isolate, to_ffi(kj::mv(source)));
}

bool allowWasmCodeGeneration(v8::Isolate* isolate) {
  v8::HandleScope scope(isolate);
  return allows_code_generation(isolate);
}

}  // namespace

WasmStreaming::WasmStreaming(v8::Isolate* isolate, std::shared_ptr<v8::WasmStreaming> streaming)
    : isolate(isolate),
      streaming(kj::mv(streaming)),
      foregroundTaskRunner(jsg::IsolateBase::from(isolate).getForegroundTaskRunner(
          v8::TaskPriority::kUserBlocking)) {}

WasmStreaming::~WasmStreaming() noexcept(false) {
  if (!done) {
    foregroundTaskRunner->PostTask(
        std::make_unique<AbortWasmStreamingTask>(isolate, kj::mv(streaming)));
  }
}

void WasmStreaming::onBytesReceived(::rust::Slice<const uint8_t> bytes) {
  KJ_ASSERT(!done);
  streaming->OnBytesReceived(bytes.data(), bytes.size());
}

void WasmStreaming::setUrl(::rust::Str url) {
  KJ_ASSERT(!done);
  // V8 requires the URL to be NUL-terminated, which a Rust string isn't.
  auto terminated = kj::str(kj::arrayPtr(url.data(), url.size()));
  streaming->SetUrl(terminated.cStr(), terminated.size());
}

void WasmStreaming::finish() {
  KJ_ASSERT(!done);
  done = true;
  streaming->Finish({});
}

void WasmStreaming::abort(::workerd::rust::jsg::Local exception) {
  KJ_ASSERT(!done);
  done = true;
  streaming->Abort(local_from_ffi<v8::Value>(kj::mv(exception)));
}

kj::Arc<ForegroundTaskSignal> foreground_task_signal(::workerd::rust::jsg::Isolate* isolate) {
  return jsg::IsolateBase::from(isolate).getForegroundTaskSignal();
}

uint64_t foreground_task_signal_epoch(ForegroundTaskSignal& signal) {
  return signal.epoch();
}

void foreground_task_signal_notify(ForegroundTaskSignal& signal) {
  signal.notify();
}

kj::Promise<void> foreground_task_signal_wait_after(ForegroundTaskSignal& signal, uint64_t seen) {
  return signal.waitAfter(seen);
}

kj::Own<WasmSubmission> submit_wasm_module(::rust::Box<ModuleBytes> bytes) {
  auto slice = bytes->as_slice();
  auto array = kj::arrayPtr(slice.data(), slice.size()).attach(kj::mv(bytes));
  return kj::heap<WasmSubmission>(
      IoContext::current().getIoChannelFactory().submitWasmModule(kj::mv(array)));
}

kj::Promise<void> wasm_submission_wait(kj::Own<WasmSubmission> submission) {
  return kj::mv(submission->accepted);
}

void abort_isolate_for_refused_module() {
  IoContext::current().abortIsolate(
      "The embedder refused a WebAssembly module that new WebAssembly.Module() already compiled."_kj);
}

void install_wasm_callbacks(::workerd::rust::jsg::Isolate* isolate) {
  // V8 installs WebAssembly.compileStreaming() and instantiateStreaming() only when this is set.
  isolate->SetWasmStreamingCallback(&wasmStreamingCallback);
  isolate->SetWasmModuleCallback(&wasmModuleCallback);
  jsg::IsolateBase::from(isolate).setAllowWasmCodeGenerationHook(&allowWasmCodeGeneration);
}

}  // namespace workerd::api::wasm
