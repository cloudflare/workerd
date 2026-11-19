// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What a startup snapshot stores of a resource belongs to no isolate, so it can't hold a GC
// reference: a `Traced` type that does not claim `HoldsNoGcReferences` is refused even when it is
// `Send` and `Sync`. A resource takes its references back with `snapshot_relink` instead.
use jsg_macros::jsg_resource;

/// Traced by hand, so it may hold a GC reference.
struct MaybeHoldsReferences;

impl jsg::Traced for MaybeHoldsReferences {
    fn trace(&self, _visitor: &mut jsg::v8::GcVisitor) {}
}

#[jsg_resource]
struct Holder;

#[jsg_resource]
impl Holder {}

impl jsg::SnapshotTransfer for Holder {
    type Snapshot = MaybeHoldsReferences;
    //~^ ERROR E0277: the trait bound `MaybeHoldsReferences: HoldsNoGcReferences` is not satisfied

    fn snapshot(&self) -> MaybeHoldsReferences {
        MaybeHoldsReferences
    }

    fn restore(_snapshot: &MaybeHoldsReferences) -> Self {
        Self
    }
}
