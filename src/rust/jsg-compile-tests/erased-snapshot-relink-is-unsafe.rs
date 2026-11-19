// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The type-erased relink reads each entry of `restored` as the address of a `Wrappable`, so safe
// code can't call it with addresses of its own.
use jsg::ErasedSnapshotTransfer;

fn relink(transfer: &dyn ErasedSnapshotTransfer, lock: &mut jsg::Lock<'_>) {
    transfer.snapshot_relink(lock, &[8]);
    //~^ ERROR E0133
}
