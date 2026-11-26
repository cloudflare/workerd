// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue"
)]

//! The bridge to the C++ shim in `ffi.h`, which wraps workerd's `SqliteDatabase`.
//!
//! The bridge is visible to all, but only so that other bridges can name `SqliteVfs`
//! (`type SqliteVfs = sqlite_rs::SqliteVfs;`). `Database`, `Query`, `Bindings` and the functions
//! are the crate's internals, not an API to use directly.
//!
//! Every function that can throw returns `Result`. The shim refuses to step a query that is done
//! or whose last step failed, which SQLite would start over, and to read a column of such a query
//! or a column that does not exist, for which SQLite returns unspecified values.

#[cxx::bridge(namespace = "workerd::rust::sqlite_rs")]
pub mod bridge {
    /// [`crate::OpenMode`] across the bridge. A cxx enum is a struct of constants, which a
    /// `match` cannot cover, so the public API has a Rust enum of its own.
    #[repr(u8)]
    enum OpenMode {
        ReadOnly,
        ReadWrite,
        Create,
        CreateWithParents,
    }

    unsafe extern "C++" {
        include!("workerd/rust/sqlite-rs/ffi.h");

        /// workerd's `SqliteDatabase::Vfs`: a SQLite VFS over a `kj::Directory`.
        type SqliteVfs;
        type Database;
        type Statement;
        type Query;
        type Bindings;

        // VFS
        fn vfs_new_in_memory() -> Result<KjOwn<SqliteVfs>>;
        fn vfs_new_on_disk(path: &str) -> Result<KjOwn<SqliteVfs>>;

        // Database
        fn database_open(vfs: &SqliteVfs, path: &str, mode: OpenMode) -> Result<KjOwn<Database>>;
        fn database_run(
            db: &Database,
            sql: &str,
            bindings: KjOwn<Bindings>,
        ) -> Result<KjOwn<Query>>;
        fn database_prepare(db: &Database, sql: &str) -> Result<KjOwn<Statement>>;

        // Statement
        fn statement_run(
            statement: Pin<&mut Statement>,
            bindings: KjOwn<Bindings>,
        ) -> Result<KjOwn<Query>>;

        // Bindings
        fn bindings_new(capacity: usize) -> KjOwn<Bindings>;
        fn bindings_add_null(bindings: Pin<&mut Bindings>);
        fn bindings_add_i64(bindings: Pin<&mut Bindings>, value: i64);
        fn bindings_add_f64(bindings: Pin<&mut Bindings>, value: f64);
        fn bindings_add_text(bindings: Pin<&mut Bindings>, value: &str);
        fn bindings_add_blob(bindings: Pin<&mut Bindings>, value: &[u8]);

        // Query
        fn query_is_done(query: &Query) -> bool;
        fn query_next(query: Pin<&mut Query>) -> Result<()>;
        fn query_change_count(query: &Query) -> Result<u64>;
        fn query_column_count(query: &Query) -> Result<u32>;
        fn query_is_null(query: &Query, column: u32) -> Result<bool>;
        fn query_i64(query: &Query, column: u32) -> Result<i64>;
        fn query_f64(query: &Query, column: u32) -> Result<f64>;
        // Copies of the value: SQLite keeps its bytes only until the next getter of the same
        // column or the next step.
        fn query_text(query: &Query, column: u32) -> Result<Vec<u8>>;
        fn query_blob(query: &Query, column: u32) -> Result<Vec<u8>>;
    }

    extern "Rust" {
        // Copies a value's bytes for `query_text` and `query_blob` to return, in one copy.
        fn bytes_to_vec(bytes: &[u8]) -> Vec<u8>;
    }
}

fn bytes_to_vec(bytes: &[u8]) -> Vec<u8> {
    bytes.to_vec()
}
