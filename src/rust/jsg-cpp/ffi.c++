// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/rust/jsg-cpp/ffi.rs.h>
#include <workerd/rust/jsg-cpp/jsg-cpp.h>

namespace workerd::rust::jsg_cpp {

kj::Own<CppRef> cpp_ref_clone(const CppRef& ref) {
  return ref.clone();
}

size_t cpp_ref_object(const CppRef& ref) {
  return reinterpret_cast<size_t>(ref.untypedObject());
}

void cpp_ref_visit(GcVisitor* visitor, const CppRef& ref) {
  ref.visitForGc(*::workerd::rust::jsg::gc_visitor_from_ffi(visitor));
}

void cpp_ref_track(
    ::workerd::rust::jsg::MemoryTracker& tracker, const char* edge_name, const CppRef& ref) {
  ref.trackField(tracker, ::workerd::rust::jsg::memoryTrackerEdgeName(edge_name));
}

void cpp_ref_detach(const CppRef& ref) {
  ref.detach();
}

bool resolve_internal_module(Isolate* isolate, ::rust::Str specifier, MaybeLocal& module) {
  return callCpp(isolate, [&](::workerd::jsg::Lock& js) {
    // The registries return the namespace as a jsg::JsObject, the tier's one JS value handle
    // (cpp-jsg-allowlist.txt), which leaves C++ JSG here as a V8 handle. The type is written out,
    // not `auto`, so that the allowlist check counts the use.
    kj::Maybe<::workerd::jsg::JsObject> found =
        js.resolveInternalModule(kj::str(kj::from<kj_rs::Rust>(specifier)));
    KJ_IF_SOME(namespaceObject, found) {
      v8::Local<v8::Object> handle = namespaceObject;
      module = ::workerd::rust::jsg::maybe_local_to_ffi(v8::MaybeLocal<v8::Object>(handle));
    }
  });
}

}  // namespace workerd::rust::jsg_cpp
