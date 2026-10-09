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
    for (int i = 0; i < trace->GetFrameCount(); ++i) {
      auto frame = trace->GetFrame(isolate, i);
      v8::String::Utf8Value function(isolate, frame->GetFunctionName());
      v8::String::Utf8Value script(isolate, frame->GetScriptName());
      builder.addFrame(toArray(function), toArray(script), frame->GetScriptId(),
          static_cast<uint32_t>(frame->GetLineNumber()), static_cast<uint32_t>(frame->GetColumn()));
    }
  }

 private:
  v8::Isolate* isolate;
  int maxFrames;

  // An empty handle (an anonymous function, a script without a name) converts to "".
  static kj::ArrayPtr<const char> toArray(const v8::String::Utf8Value& value) {
    if (*value == nullptr) return nullptr;
    return kj::arrayPtr(*value, value.length());
  }
};

}  // namespace

kj::Arc<const AsyncStackCapturer> newAsyncStackCapturer(v8::Isolate* isolate, uint32_t maxFrames) {
  return kj::arc<V8StackCapturer>(isolate, maxFrames);
}

}  // namespace workerd
