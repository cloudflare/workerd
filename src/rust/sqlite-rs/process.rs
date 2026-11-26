// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::num::NonZeroUsize;

use cxx::KjError;
use cxx::KjExceptionType;

use crate::Result;
use crate::ffi::bridge;

/// Sets the most memory that SQLite may allocate in the whole process.
///
/// `None` removes the limit, and the call returns the limit it replaces. An allocation past the
/// limit fails, and so does the call that made it, with `SQLITE_NOMEM`, whichever database it is
/// for.
///
/// The limit is SQLite's hard heap limit (`sqlite3_hard_heap_limit64()`). It applies at once, to
/// the databases already open and to those opened later, and the latest call wins. The call also
/// sets the 128 MiB soft limit, under which SQLite frees cache, that workerd's `SqliteDatabase`
/// sets when the process opens its first database; SQLite lowers it to a lower hard limit.
///
/// Databases that this crate opens leave the hard limit alone. A C++ `SqliteDatabase` sets it once,
/// from the `sqliteMaxMemoryPerProcessBytes` of the first database the process opens, unless that
/// is `kj::maxValue`; if that database opens after this call, its limit replaces this one.
///
/// The call initializes SQLite, as creating the first [`Vfs`] does, so a process that installs a
/// custom allocator must install it first.
///
/// [`Vfs`]: crate::Vfs
pub fn set_max_memory_per_process_bytes(
    bytes: Option<NonZeroUsize>,
) -> Result<Option<NonZeroUsize>> {
    let bytes = bytes
        .map_or(Ok(0), |bytes| i64::try_from(bytes.get()))
        .map_err(|_| {
            KjError::new(
                KjExceptionType::Failed,
                "the per-process memory limit is out of range".to_owned(),
            )
        })?;
    let prior = bridge::set_max_memory_per_process_bytes(bytes)?;
    // SQLite returns a limit that was set, which fits in `usize`, or 0 for none.
    Ok(usize::try_from(prior).ok().and_then(NonZeroUsize::new))
}

#[cfg(test)]
#[path = "process-test.rs"]
mod tests;
