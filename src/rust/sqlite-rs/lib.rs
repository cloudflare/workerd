// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! SQLite for Rust through workerd's C++ wrapper, `SqliteDatabase` (`workerd/util/sqlite.h`).
//!
//! Every call into SQLite is made by that wrapper, so it does what the wrapper does around each
//! one: it enters the database's memory scope (which the custom allocator of
//! `installSqliteCustomAllocator()`, when installed, charges), and it catches an exception that a
//! VFS method throws and returns it as the call's error, in place of SQLite's bare error code.
//! The crate links the binary's one SQLite library (`@sqlite3`) and never configures it (no
//! `sqlite3_config()`). Its first use initializes SQLite (creating a [`Vfs`] registers one, and
//! opening a database opens a connection), so a process that installs a custom allocator, such as
//! `installSqliteCustomAllocator()`, must install it before it creates its first `Vfs`.
//!
//! A [`Vfs`] is the directory that databases are opened in. A [`Database`] is a handle to a
//! connection, which holds its `Vfs`; a [`Statement`] holds its database, so the two can be
//! stored side by side in any order, and the connection is closed after the last of them. [`Rows`]
//! borrow what they run on. None of them is `Send`.
//!
//! # Dropping
//!
//! Dropping [`Rows`], a [`Statement`], the last handle of a [`Database`] or the last handle of a
//! [`Vfs`] runs the C++ destructor (`~Query`, `~Statement`, `~SqliteDatabase`, `~Vfs`) from
//! Rust's `Drop`, through cxx. Those destructors are `noexcept(false)`, but an exception cannot
//! unwind into Rust, so one thrown there would abort the process. None throws for what this crate
//! exposes: it offers no `reset()` or close of a database, and `~SqliteDatabase` falls back to
//! SQLite's deferred close (`sqlite3_close_v2()`) rather than fail while statements remain. An
//! error during a drop, such as a VFS exception while SQLite finishes a file on close, has no
//! caller to go to, so it is logged (`sqliteVfsError`) and not returned.

mod database;
mod ffi;
mod vfs;

pub use database::Database;
pub use database::OpenMode;
pub use database::Param;
pub use database::Rows;
pub use database::Statement;
/// workerd's C++ `SqliteDatabase::Vfs`, for another bridge to name (`type SqliteVfs =
/// sqlite_rs::SqliteVfs;`) when C++ hands one to Rust; see [`Vfs::from_kj`].
pub use ffi::bridge::SqliteVfs;
pub use vfs::Vfs;

/// The error of every fallible call: the C++ exception, with its type, description and details.
pub type Result<T> = std::result::Result<T, cxx::KjError>;
