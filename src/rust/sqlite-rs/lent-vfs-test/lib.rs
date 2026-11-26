// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A VFS that C++ owns and lends to Rust with [`sqlite_rs::Vfs::from_kj`], as C++ storage does:
//! the test of how long such an owner lives. The bridge is separate from `sqlite-rs`'s, since the
//! crate's own test binary cannot link the library build of the crate as well.

#[cxx::bridge(namespace = "workerd::rust::sqlite_rs::lent_vfs_test")]
pub mod ffi {
    #[namespace = "workerd::rust::sqlite_rs"]
    unsafe extern "C++" {
        include!("workerd/rust/sqlite-rs/ffi.h");
        type SqliteVfs = sqlite_rs::SqliteVfs;
    }

    unsafe extern "C++" {
        include!("workerd/rust/sqlite-rs/lent-vfs-test/ffi.h");

        /// A new refcounted owner's VFS (over an in-memory directory), through a `kj::Own` that
        /// holds the only reference to the owner.
        fn vfs_lent_by_new_owner() -> Result<KjOwn<SqliteVfs>>;
        /// How many owners on this thread are alive.
        fn vfs_owners_alive() -> usize;
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
