// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::rc::Rc;

use kj_rs::KjOwn;

use crate::Result;
use crate::ffi::bridge;
use crate::ffi::bridge::SqliteVfs;

/// How a [`Vfs`] treats its files: workerd's `SqliteDatabase::VfsOptions`. The default is that
/// of `SqliteDatabase::Vfs`, which [`Vfs::in_memory`] and [`Vfs::on_disk`] use.
#[derive(Clone, Copy, Debug)]
pub struct VfsOptions(bridge::VfsSettings);

impl Default for VfsOptions {
    fn default() -> Self {
        Self(bridge::vfs_default_settings())
    }
}

impl VfsOptions {
    /// The default options.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The `SQLITE_IOCAP_*` flags that the VFS reports as its files' device characteristics, in
    /// place of the default, `SQLITE_IOCAP_POWERSAFE_OVERWRITE`. An on-disk VFS reports them
    /// combined with the native VFS's by a bitwise OR, except on Windows, where `SqliteDatabase`
    /// opens a database on disk through SQLite's own VFS, which ignores them.
    #[must_use]
    pub const fn device_characteristics(mut self, flags: i32) -> Self {
        self.0.device_characteristics = flags;
        self
    }

    /// Makes syncs (`fsync()` and the like) succeed without doing anything. A crash of the
    /// machine may then lose or corrupt recent commits; a crash of the process alone cannot. For
    /// databases that need not survive the machine, such as those of tests.
    #[must_use]
    pub const fn without_sync(mut self) -> Self {
        self.0.sync = false;
        self
    }
}

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
        Self::in_memory_with(VfsOptions::default())
    }

    /// [`Vfs::in_memory`] with `options`.
    pub fn in_memory_with(options: VfsOptions) -> Result<Self> {
        Ok(Self::from_kj(bridge::vfs_new_in_memory(options.0)?))
    }

    /// A VFS over the existing directory at `path`, absolute or relative to the current
    /// directory. SQLite opens its files with its native VFS, as workerd does for on-disk storage.
    pub fn on_disk(path: &str) -> Result<Self> {
        Self::on_disk_with(path, VfsOptions::default())
    }

    /// [`Vfs::on_disk`] with `options`.
    pub fn on_disk_with(path: &str, options: VfsOptions) -> Result<Self> {
        Ok(Self::from_kj(bridge::vfs_new_on_disk(path, options.0)?))
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
