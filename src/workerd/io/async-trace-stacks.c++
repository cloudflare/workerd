// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-trace-stacks.h"

#include <v8-debug.h>
#include <v8-isolate.h>
#include <v8-locker.h>
#include <v8-primitive.h>

namespace workerd {
namespace {

// Deeper stacks cost more to capture than they help.
constexpr uint32_t MAX_FRAMES = 256;

class V8StackCapturer final: public AsyncStackCapturer {
 public:
  V8StackCapturer(v8::Isolate* isolate, uint32_t maxFrames)
      : isolate(isolate),
        maxFrames(static_cast<int>(kj::min(maxFrames, MAX_FRAMES))) {}

  void capture(AsyncStackBuilder& builder) const override {
    if (v8::Isolate::TryGetCurrent() != isolate || !v8::Locker::IsLocked(isolate)) return;
    v8::HandleScope scope(isolate);
    auto trace = v8::StackTrace::CurrentStackTrace(isolate, maxFrames,
        static_cast<v8::StackTrace::StackTraceOptions>(v8::StackTrace::kColumnOffset |
            v8::StackTrace::kScriptName | v8::StackTrace::kFunctionName |
            v8::StackTrace::kScriptId));
    int count = trace->GetFrameCount();
    for (int i = 0; i < count; ++i) {
      auto frame = trace->GetFrame(isolate, i);
      builder.addFramePosition(frame->GetScriptId(), static_cast<uint32_t>(frame->GetLineNumber()),
          static_cast<uint32_t>(frame->GetColumn()));
    }
    // Names are needed only for a stack the isolate doesn't keep yet.
    if (builder.isKnown()) return;
    for (int i = 0; i < count; ++i) {
      auto frame = trace->GetFrame(isolate, i);
      auto function = toUtf8(frame->GetFunctionName());
      auto script = toUtf8(frame->GetScriptName());
      builder.addFrame(function, script, frame->GetScriptId(),
          static_cast<uint32_t>(frame->GetLineNumber()), static_cast<uint32_t>(frame->GetColumn()));
    }
  }

 private:
  v8::Isolate* isolate;
  int maxFrames;

  // `value` as UTF-8, with U+FFFD for each lone surrogate. An empty handle (an anonymous
  // function, a script without a name) converts to "".
  kj::Array<char> toUtf8(v8::Local<v8::String> value) const {
    if (value.IsEmpty()) return nullptr;
    auto utf8 = kj::heapArray<char>(value->Utf8Length(isolate));
    auto written = value->WriteUtf8(
        isolate, utf8.begin(), utf8.size(), v8::String::WriteFlags::kReplaceInvalidUtf8);
    KJ_ASSERT(written == utf8.size(), "WriteUtf8 must completely overwrite the backing buffer");
    return utf8;
  }
};

}  // namespace

kj::Arc<const AsyncStackCapturer> newAsyncStackCapturer(v8::Isolate* isolate, uint32_t maxFrames) {
  return kj::arc<V8StackCapturer>(isolate, maxFrames);
}

}  // namespace workerd
