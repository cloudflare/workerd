// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Types describing a V8 startup snapshot and how an isolate relates to one. Only high-level code
// that creates isolates (jsg::IsolateBase and its callers) needs to include this file.

#include <v8-snapshot.h>

#include <kj/array.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd::jsg {

// Everything needed to create a new isolate from a V8 startup snapshot:
// * `blob` is the serialized snapshot data, allocated with `new[]` by v8::SnapshotCreator.
// * `externalReferences` is the list of addresses of all C++ functions and objects referenced
//   from the snapshot. The producer and the consumer of a snapshot must register exactly the
//   same references, in the same order, or deserialization will fail.
// * `externalReferenceCursor` is the next free slot in `externalReferences`.
struct SnapshotArtifact: public kj::AtomicRefcounted {
  v8::StartupData blob{nullptr, 0};
  kj::Array<intptr_t> externalReferences;
  size_t externalReferenceCursor = 0;

  // The zygote's resource-type function templates, stored in the blob with
  // v8::SnapshotCreator::AddData(): one index per slot visited by
  // IsolateBase::iterateResourceTypeTemplates(), kNoTemplateData for a template the zygote never
  // created, plus the opaque-object template. Every JS object in the snapshot was instantiated
  // from these templates, and JSG unwraps by template identity (FindInstanceInPrototypeChain),
  // so an isolate restored from the blob must adopt them (IsolateBase::adoptTemplatesFromSnapshot)
  // rather than create fresh ones: otherwise `new Response()` from the snapshot's global fails to
  // unwrap, snapshotted binding objects reject their own methods and `instanceof` breaks.
  static constexpr size_t kNoTemplateData = SIZE_MAX;
  kj::Array<size_t> templateDataIndices;
  size_t opaqueTemplateDataIndex = kNoTemplateData;

  // The new module registry's resolution table (modules::IsolateModuleRegistry): one record per
  // (context type, specifier) the zygote resolved, naming the v8::Module it resolved to by its
  // v8::SnapshotCreator::AddData(context, ...) index. Several specifiers may name one module.
  // A restored isolate rebuilds its registry from these (ModuleRegistry::attachToIsolate); without
  // them the registry is empty, and V8's import.meta callback, which looks the module up in the
  // registry, would leave import.meta.url and import.meta.main unset.
  struct ModuleRecord {
    uint8_t contextType;  // A modules::ResolveContext::Type.
    kj::String specifier;  // The normalized specifier URL, query and fragment included.
    size_t moduleDataIndex;
  };
  kj::Array<ModuleRecord> moduleRecords;

  ~SnapshotArtifact() noexcept(false) {
    // v8::SnapshotCreator::CreateBlob() allocates the data with `new[]` and hands over ownership.
    delete[] blob.data;
  }

  kj::Arc<SnapshotArtifact> addRef() const {
    return addRefToThis();
  }
};

// Different views for snapshot artifacts:
// * MutableSnapshot: a zygote isolate built via v8::SnapshotCreator; holds an owning ref to
//   the artifact and fills it in IsolateBase::createSnapshotBlob().
// * FinalizedSnapshot: an isolate that boots from a previously produced blob; the Arc
//   keeps the artifact alive for the isolate's entire life.
struct MutableSnapshot {
  kj::Own<SnapshotArtifact> artifact;
};
struct FinalizedSnapshot {
  kj::Arc<SnapshotArtifact> artifact;
};
using SnapshotConfig = kj::OneOf<MutableSnapshot, FinalizedSnapshot>;

}  // namespace workerd::jsg
