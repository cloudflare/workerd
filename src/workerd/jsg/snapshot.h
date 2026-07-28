// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Types describing a V8 startup snapshot and how an isolate relates to one. Only high-level code
// that creates isolates (jsg::IsolateBase and its callers) needs to include this file.

#include <v8-snapshot.h>

#include <kj/one-of.h>
#include <kj/refcount.h>

namespace workerd::jsg {

// Everything needed to create a new isolate from a V8 startup snapshot:
// * `blob` is the serialized snapshot data, allocated with `new[]` by v8::SnapshotCreator.
struct SnapshotArtifact: public kj::AtomicRefcounted {
  v8::StartupData blob{nullptr, 0};

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
//   the artifact and fills it during IsolateBase::prepareSnapshot().
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
