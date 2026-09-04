// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// Compile-time assignment of per-type CppHeapPointerTags to JSG resource types, so that
// unwrapping a JS wrapper into its C++ object can range-check the type through V8's CppHeap
// pointer table. See ept-tagging design notes.
//
// The scheme mirrors what V8's CppHeapPointerTag enum documents: assign every resource type a
// dense id via a DFS pre-order over the JSG_INHERIT forest, so that the ids of a type and all of
// its (transitive) subclasses form a contiguous interval. A method receiver of static type T then
// accepts any wrapper whose tag lies in T's subtree interval -- i.e. T or any subclass -- and
// rejects everything else.

#include <workerd/jsg/util.h>  // JsgKind

#include <v8-sandbox.h>  // CppHeapPointerTag, CppHeapPointerTagRange

#include <cstdint>

namespace workerd::jsg {

// The tag reserved for wrappables that are not JSG resource types: WrappableFunction<Sig> (the
// backing object of a jsg::Function) and opaque wrappers. Resource types receive their own
// per-type tags, numbered starting immediately after this one.
//
// This single shared tag does not by itself pin down a concrete C++ type -- it
// only distinguishes "non-resource wrappable" from "resource". Polymorphic
// unwrap sites that use it will typically follow up with a dynamic_cast or other
// vtable-backed techniques that pin down the exact type based on non-sandbox data.
constexpr uint16_t kNonResourceWrappableTag =
    static_cast<uint16_t>(v8::CppHeapPointerTag::kFirstObjectWrappableTag);

// The dense id of the first resource type. Resource ids are offset past the non-resource tag so
// that the two spaces never collide within kObjectWrappableTagRange.
constexpr uint16_t kFirstResourceTag = kNonResourceWrappableTag + 1;

// Upper bound on the number of distinct wrappable tags any single isolate type may use: one
// catch-all tag plus one per registered resource type. It sizes the freelist bucket array in
// HeapTracer (a small fixed array, indexed by tag) and is enforced at compile time by a
// static_assert in TypeWrapper::wrappableTag<T>().
//
// To grow it: just raise this number. The only cost is a slightly larger fixed array per isolate
// (kMaxWrappableTags pointers). There is no correctness subtlety -- the bound exists solely so the
// runtime array can be statically sized rather than grown on demand. Real isolates use on the
// order of ~270 tags today.
constexpr uint16_t kMaxWrappableTags = 310;

// Index of a tag within the freelist bucket array: tags are dense starting at
// kFirstObjectWrappableTag (the catch-all), so the offset is a compact array index.
constexpr uint16_t wrappableTagBucketIndex(uint16_t tag) {
  return tag - kNonResourceWrappableTag;
}

// The single-tag range used to unwrap non-resource wrappables (functions, opaque wrappers), which
// all share kNonResourceWrappableTag.
constexpr v8::CppHeapPointerTagRange kNonResourceWrappableTagRange(
    static_cast<v8::CppHeapPointerTag>(kNonResourceWrappableTag),
    static_cast<v8::CppHeapPointerTag>(kNonResourceWrappableTag));

}  // namespace workerd::jsg
