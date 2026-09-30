#include "sandbox.h"

#include <v8-isolate.h>

#include <kj/debug.h>

#include <cstdint>
#include <cstdlib>

namespace workerd::jsg {

bool violatesSandbox(v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> range) {
#ifdef V8_ENABLE_SANDBOX
  if (range.size() == 0) return false;
  // The range may be corrupted, so avoid pointer arithmetic and reject address wraparound.
  auto first = reinterpret_cast<uintptr_t>(range.begin());
  auto last = first + (range.size() - 1);
  if (last < first) return true;
  auto group = isolate->GetGroup();
  // The sandbox is one contiguous reservation, so checking both ends covers the whole range.
  return !group.SandboxContains(reinterpret_cast<void*>(first)) ||
      !group.SandboxContains(reinterpret_cast<void*>(last));
#else
  return false;
#endif
}

void abortOnSandboxViolation(
    v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> range, kj::StringPtr what) {
  if (!violatesSandbox(isolate, range)) return;
  // Abort rather than throw: edgeworker's crash handler turns this into an abrupt shutdown of
  // the isolate, and the message identifies the event in the crash report.
  KJ_LOG(FATAL, "native memory outside the V8 sandbox reached a copy-out sink", what, range.begin(),
      range.size());
  abort();
}

}  // namespace workerd::jsg
