// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Startup-snapshot support for Rust resources.
//!
//! An isolate restored from a startup snapshot gets the zygote's JavaScript heap, including the
//! wrapper objects of Rust resources that the zygote's top-level code retained (a builtin module's
//! exports, say), but not their Rust halves. A resource type that is fully re-created by a
//! constructor-like function implements [`SnapshotRestore`] and is registered with [`register`];
//! the zygote then records the type's name in each such wrapper, and the restored isolate
//! re-creates the resource and attaches it to the deserialized wrapper. This mirrors
//! `JSG_SNAPSHOT_RESTORE` for C++ resource types (see `workerd/jsg/jsg.h`). A retained wrapper of
//! a type that is not registered makes the snapshot fail.

use std::sync::Mutex;
use std::sync::PoisonError;

use crate::Resource;
use crate::resource::Rc;
use crate::v8;

/// A resource type whose instances can be re-created in an isolate restored from a snapshot.
///
/// The zygote's instance is dropped; `restore_from_snapshot` must produce an equivalent one, so
/// only types whose whole state is what their constructor computes qualify.
pub trait SnapshotRestore: Resource {
    fn restore_from_snapshot() -> Rc<Self>;
}

struct Restorer {
    /// `TypeId` halves as stored in the C++ `Wrappable` (see `TraitObjectPtr::from_raw`).
    type_id: [usize; 2],
    name: &'static str,
    restore: unsafe fn(v8::IsolatePtr, v8::ffi::Local),
}

/// Process-wide: the zygote and the isolates restored from it live in the same process, and the
/// registered types are the same for every isolate.
static RESTORERS: Mutex<Vec<Restorer>> = Mutex::new(Vec::new());

fn type_id_halves<R: 'static>() -> [usize; 2] {
    // SAFETY: TypeId is 128 bits, stored as two usize halves exactly as TraitObjectPtr does.
    unsafe { std::mem::transmute(std::any::TypeId::of::<R>()) }
}

/// Registers `R` as restorable from a startup snapshot. Idempotent.
pub fn register<R: SnapshotRestore>() {
    let type_id = type_id_halves::<R>();
    let mut restorers = RESTORERS.lock().unwrap_or_else(PoisonError::into_inner);
    if restorers.iter().any(|r| r.type_id == type_id) {
        return;
    }
    restorers.push(Restorer {
        type_id,
        name: std::any::type_name::<R>(),
        restore: restore_resource::<R>,
    });
}

unsafe fn restore_resource<R: SnapshotRestore>(isolate: v8::IsolatePtr, holder: v8::ffi::Local) {
    let resource = R::restore_from_snapshot();
    // SAFETY: the caller passes a live, locked isolate and a deserialized wrapper of R's template.
    unsafe { resource.attach_to_object(isolate, holder) };
}

/// The recipe for a wrappable whose Rust object has the given `TypeId` halves: the type's name if
/// it is registered, or the empty string (not restorable).
pub(crate) fn recipe_for(type_id: [usize; 2]) -> &'static str {
    let restorers = RESTORERS.lock().unwrap_or_else(PoisonError::into_inner);
    restorers
        .iter()
        .find(|r| r.type_id == type_id)
        .map_or("", |r| r.name)
}

/// Re-creates the resource named `name` and attaches it to `holder`. Returns false if no
/// registered type has that name.
///
/// # Safety
/// `isolate` must be live and locked, with a context entered; `holder` must be a wrapper
/// deserialized from a snapshot whose zygote recorded `name` for it.
pub(crate) unsafe fn restore(isolate: v8::IsolatePtr, holder: v8::ffi::Local, name: &str) -> bool {
    let restore = {
        let restorers = RESTORERS.lock().unwrap_or_else(PoisonError::into_inner);
        match restorers.iter().find(|r| r.name == name) {
            Some(r) => r.restore,
            None => return false,
        }
    };
    // SAFETY: forwarded from the caller. The registry lock is released first: the restore
    // function allocates a resource, which must not run under it.
    unsafe { restore(isolate, holder) };
    true
}
