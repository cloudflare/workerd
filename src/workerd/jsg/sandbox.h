#pragma once

#include <kj/common.h>
#include <kj/string.h>

namespace v8 {
class Isolate;
}

namespace workerd::jsg {

// Whether any byte of `range` lies outside the isolate group's V8 sandbox. Empty ranges do not
// violate it. Returns false when V8 is built without the sandbox.
bool violatesSandbox(v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> range);

// Aborts the process if `range` violates the V8 sandbox; does nothing without the sandbox.
// For native code that copies bytes out of memory a JavaScript-visible object claims is backed
// by a V8 ArrayBuffer. Such bytes must live in the sandbox, so a violation means the object was
// reached through corrupted state, and copying would disclose memory belonging to someone else.
// `what` names the memory in the crash report.
void abortOnSandboxViolation(
    v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> range, kj::StringPtr what);

}  // namespace workerd::jsg
