#pragma once

#include <kj/common.h>
#include <kj/string.h>

namespace v8 {
class Isolate;
}

namespace workerd::jsg {

// Whether every byte of `range` lies inside the V8 sandbox of the isolate's group. An empty
// range counts as inside. Always true when V8 is built without the sandbox.
bool isInsideSandbox(v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> range);

// Aborts the process unless `range` is inside the sandbox. For native code that copies bytes
// out of memory a JavaScript-visible object claims is backed by a V8 ArrayBuffer. Such bytes
// must live in the sandbox, so a failure means the object was reached through corrupted state, and
// copying would disclose memory belonging to someone else. `what` names the memory in the
// crash report.
void requireInsideSandbox(
    v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> range, kj::StringPtr what);

}  // namespace workerd::jsg
