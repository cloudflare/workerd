// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::rc::Rc;

use kj_rs::KjOwn;

use crate::Result;
use crate::ffi::bridge;
use crate::ffi::bridge::SqliteVfs;

/// A SQLite VFS: workerd's `SqliteDatabase::Vfs` over a directory.
///
/// Every database opened through it reads and writes that directory. A clone is another handle
/// to the same VFS, and each [`Database`] holds one, so the VFS lives as long as any database
/// opened through it.
///
/// [`Database`]: crate::Database
#[derive(Clone)]
pub struct Vfs {
    inner: Rc<KjOwn<SqliteVfs>>,
}

impl Vfs {
    /// A VFS over a new, empty in-memory directory. Its files last as long as the VFS.
    pub fn in_memory() -> Result<Self> {
        Ok(Self::from_kj(bridge::vfs_new_in_memory()?))
    }

    /// A VFS over the existing directory at `path`, absolute or relative to the current
    /// directory. SQLite opens its files with its native VFS, as workerd does for on-disk storage.
    pub fn on_disk(path: &str) -> Result<Self> {
        Ok(Self::from_kj(bridge::vfs_new_on_disk(path)?))
    }

    /// The VFS that C++ hands over. To lend Rust a VFS that it owns elsewhere, C++ passes a
    /// non-owning `kj::Own` with the owner attached, which the handle then keeps alive.
    #[must_use]
    pub fn from_kj(vfs: KjOwn<SqliteVfs>) -> Self {
        Self {
            inner: Rc::new(vfs),
        }
    }

    pub(crate) fn get(&self) -> &SqliteVfs {
        &self.inner
    }
}

#[cfg(test)]
#[path = "vfs-test.rs"]
mod tests;
