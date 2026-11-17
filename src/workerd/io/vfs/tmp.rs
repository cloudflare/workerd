// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::RefCell;
use std::rc::Rc;

use crate::Directory;
use crate::VfsHost;

/// The contents of `/tmp`, for one request or for a scope outside any request. Cloning a store
/// adds a reference to the same store.
///
/// A request's store belongs to its `IoContext` and is found through
/// [`VfsHost::io_context_tmp_store`]. Outside a request, as when a Worker's top level is
/// evaluated, the store is the one a [`TmpDirStoreScope`] has made current on the thread.
/// Either way, the files go away with the store.
#[derive(Clone)]
pub struct TmpDirStore(Rc<TmpStoreInner>);

struct TmpStoreInner {
    dir: Directory,
}

impl Default for TmpDirStore {
    fn default() -> Self {
        Self::new()
    }
}

impl TmpDirStore {
    /// A new store with an empty directory.
    pub fn new() -> Self {
        Self(Rc::new(TmpStoreInner {
            dir: Directory::new_writable(),
        }))
    }

    /// The directory holding the store's files.
    pub fn directory(&self) -> Directory {
        self.0.dir.clone()
    }

    /// Returns true if this and `other` are the same store.
    pub fn is_same(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }
}

thread_local! {
    static CURRENT: RefCell<Option<TmpDirStore>> = const { RefCell::new(None) };
}

/// Makes a new [`TmpDirStore`] current on this thread for as long as the scope lives. At most
/// one scope is current on a thread at a time.
pub struct TmpDirStoreScope {
    store: TmpDirStore,
}

impl TmpDirStoreScope {
    /// Makes a new store current, or returns `None` if a scope is already current on this thread.
    pub fn enter() -> Option<Self> {
        CURRENT.with_borrow_mut(|current| {
            if current.is_some() {
                return None;
            }
            let store = TmpDirStore::new();
            *current = Some(store.clone());
            Some(Self { store })
        })
    }

    /// The store this scope made current.
    pub const fn store(&self) -> &TmpDirStore {
        &self.store
    }

    /// The store of the scope current on this thread, if any.
    pub fn current() -> Option<TmpDirStore> {
        CURRENT.with_borrow(Clone::clone)
    }
}

impl Drop for TmpDirStoreScope {
    fn drop(&mut self) {
        CURRENT.with_borrow_mut(|current| {
            if current
                .as_ref()
                .is_some_and(|store| store.is_same(&self.store))
            {
                *current = None;
            }
        });
    }
}

/// The store `/tmp` refers to now: the current request's if there is one, and otherwise that of
/// the scope current on this thread.
pub fn current_tmp_store(host: &dyn VfsHost) -> Option<TmpDirStore> {
    host.io_context_tmp_store()
        .or_else(TmpDirStoreScope::current)
}

#[cfg(test)]
#[path = "tmp-test.rs"]
mod tests;
