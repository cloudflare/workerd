// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::marker::PhantomData;
use std::rc::Rc;

use cxx::KjError;
use cxx::KjExceptionType;
use kj_rs::KjOwn;

use crate::Result;
use crate::Vfs;
use crate::ffi::bridge;

/// How [`Database::open`] opens a database file.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum OpenMode {
    /// Read only. The file must exist.
    ReadOnly,
    /// Read and write. The file must exist.
    ReadWrite,
    /// Read and write, creating the file if it does not exist.
    Create,
    /// Read and write, creating the file and its parent directories if they do not exist.
    CreateWithParents,
}

impl From<OpenMode> for bridge::OpenMode {
    fn from(mode: OpenMode) -> Self {
        match mode {
            OpenMode::ReadOnly => Self::ReadOnly,
            OpenMode::ReadWrite => Self::ReadWrite,
            OpenMode::Create => Self::Create,
            OpenMode::CreateWithParents => Self::CreateWithParents,
        }
    }
}

/// A value bound to a `?` of a statement.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Param<'a> {
    Null,
    Integer(i64),
    Real(f64),
    Text(&'a str),
    Blob(&'a [u8]),
}

impl From<i64> for Param<'_> {
    fn from(value: i64) -> Self {
        Self::Integer(value)
    }
}

impl From<f64> for Param<'_> {
    fn from(value: f64) -> Self {
        Self::Real(value)
    }
}

impl<'a> From<&'a str> for Param<'a> {
    fn from(value: &'a str) -> Self {
        Self::Text(value)
    }
}

impl<'a> From<&'a [u8]> for Param<'a> {
    fn from(value: &'a [u8]) -> Self {
        Self::Blob(value)
    }
}

/// `None` binds NULL.
impl<T: Into<Self>> From<Option<T>> for Param<'_> {
    fn from(value: Option<T>) -> Self {
        value.map_or(Self::Null, Into::into)
    }
}

/// A connection to a SQLite database: workerd's `SqliteDatabase`, which makes every call into
/// SQLite for this crate.
///
/// A `Database` is a handle: a clone is another handle to the same connection. The connection
/// stays open while any handle or any [`Statement`] prepared on it exists, and is closed when the
/// last of them is dropped, in whatever order they are.
///
/// Queries are trusted: they may name any table, `_cf_` ones included, and run transactions.
#[derive(Clone)]
pub struct Database(Rc<Inner>);

struct Inner {
    // Declared before `vfs`, so that the connection is closed before its VFS is released.
    db: KjOwn<bridge::Database>,
    vfs: Vfs,
}

impl Database {
    /// Opens the database at `path`, relative to the VFS's directory.
    pub fn open(vfs: &Vfs, path: &str, mode: OpenMode) -> Result<Self> {
        Ok(Self(Rc::new(Inner {
            db: bridge::database_open(vfs.get(), path, mode.into())?,
            vfs: vfs.clone(),
        })))
    }

    /// The VFS the database was opened through.
    #[must_use]
    pub fn vfs(&self) -> &Vfs {
        &self.0.vfs
    }

    /// Runs `sql`, which may be several statements separated by semicolons. `params` and the
    /// returned rows belong to the last of them; the others run first, to completion.
    pub fn run(&self, sql: &str, params: &[Param<'_>]) -> Result<Rows<'_>> {
        Ok(Rows::new(bridge::database_run(
            &self.0.db,
            sql,
            bindings(params)?,
        )?))
    }

    /// Prepares `sql`, a single statement, to run any number of times. The statement holds the
    /// connection open.
    pub fn prepare(&self, sql: &str) -> Result<Statement> {
        Ok(Statement {
            inner: bridge::database_prepare(&self.0.db, sql)?,
            db: self.clone(),
        })
    }
}

/// A prepared statement, which holds its database's connection open.
pub struct Statement {
    // Declared before `db`: the C++ statement refers to the connection, so it is destroyed first.
    inner: KjOwn<bridge::Statement>,
    db: Database,
}

impl Statement {
    /// The database the statement was prepared on.
    #[must_use]
    pub const fn database(&self) -> &Database {
        &self.db
    }

    /// Runs the statement with `params`. A statement runs once at a time, so the rows borrow it.
    pub fn run(&mut self, params: &[Param<'_>]) -> Result<Rows<'_>> {
        Ok(Rows::new(bridge::statement_run(
            self.inner.as_mut(),
            bindings(params)?,
        )?))
    }
}

/// The rows of a running query, which borrow the database or statement `'a`. A query starts on
/// its first row; a statement that returns none, such as an `INSERT`, is done at once.
///
/// A getter reads the current row's column, converted as SQLite converts values (`i64` of a
/// text column parses it, `text` of NULL is empty), and fails when the query is done, when a step
/// failed, or when the column does not exist. After a failed step the query is over: further
/// steps fail too, rather than run it again.
///
/// Dropping the rows before the query is done stops it there, and an error that the rest of it
/// would have returned is lost. A statement with `RETURNING` makes its changes before its first
/// row, but outside a transaction it commits them only after its last, so dropping its rows
/// early commits them as the drop resets the statement, and a commit that fails, such as on a
/// deferred foreign key, rolls them back without a word. [`Rows::finish`] runs the query to its
/// end and returns that error.
///
/// Forgetting the rows (`std::mem::forget`) is not undefined behaviour, but leaves the query
/// running for as long as the connection is open: its statement is neither reset nor finalized,
/// so it keeps its locks and the transaction of a write, and a [`Statement`] it ran on fails to
/// run again ("can only be executed once at a time"). Closing the connection finalizes it.
pub struct Rows<'a> {
    inner: KjOwn<bridge::Query>,
    borrow: PhantomData<&'a mut ()>,
}

impl Rows<'_> {
    const fn new(inner: KjOwn<bridge::Query>) -> Self {
        Self {
            inner,
            borrow: PhantomData,
        }
    }

    /// Whether there are no more rows.
    #[must_use]
    pub fn is_done(&self) -> bool {
        bridge::query_is_done(&self.inner)
    }

    /// Advances to the next row. Fails when the query is done or a step failed.
    pub fn next_row(&mut self) -> Result<()> {
        Ok(bridge::query_next(self.inner.as_mut())?)
    }

    /// Runs the query to its end, skipping the rows left, and returns the error of a step that
    /// fails, which dropping the rows would discard.
    pub fn finish(mut self) -> Result<()> {
        while !self.is_done() {
            self.next_row()?;
        }
        Ok(())
    }

    /// The number of rows the statement inserted, updated or deleted, once it is done: 0 for one
    /// that changes no rows, such as `CREATE TABLE`, `BEGIN` or a `PRAGMA`. Fails for a query that
    /// returns rows, such as a `SELECT` or a statement with `RETURNING`.
    ///
    /// The count is this statement's even when other statements ran after it. Like SQLite's
    /// `sqlite3_changes()`, it leaves out the rows that triggers, foreign key actions and
    /// `REPLACE` conflict resolution changed.
    pub fn change_count(&self) -> Result<u64> {
        Ok(bridge::query_change_count(&self.inner)?)
    }

    /// The number of columns of each row.
    pub fn column_count(&self) -> Result<u32> {
        Ok(bridge::query_column_count(&self.inner)?)
    }

    /// Whether `column` is NULL.
    pub fn is_null(&self, column: u32) -> Result<bool> {
        Ok(bridge::query_is_null(&self.inner, column)?)
    }

    /// The integer of `column`.
    pub fn i64(&self, column: u32) -> Result<i64> {
        Ok(bridge::query_i64(&self.inner, column)?)
    }

    /// The floating-point number of `column`.
    pub fn f64(&self, column: u32) -> Result<f64> {
        Ok(bridge::query_f64(&self.inner, column)?)
    }

    /// The text of `column`. Fails when it is not UTF-8.
    pub fn text(&self, column: u32) -> Result<String> {
        let bytes = bridge::query_text(&self.inner, column)?;
        String::from_utf8(bytes).map_err(|error| {
            KjError::new(
                KjExceptionType::Failed,
                format!("column {column} is not UTF-8 text: {error}"),
            )
        })
    }

    /// The text of `column`, or `None` when it is NULL.
    pub fn maybe_text(&self, column: u32) -> Result<Option<String>> {
        if self.is_null(column)? {
            Ok(None)
        } else {
            self.text(column).map(Some)
        }
    }

    /// The bytes of `column`.
    pub fn blob(&self, column: u32) -> Result<Vec<u8>> {
        Ok(bridge::query_blob(&self.inner, column)?)
    }

    /// The blob of `column`, or `None` when it is NULL.
    pub fn maybe_blob(&self, column: u32) -> Result<Option<Vec<u8>>> {
        if self.is_null(column)? {
            Ok(None)
        } else {
            self.blob(column).map(Some)
        }
    }
}

// Dropping rows destroys the C++ query (`~Query`), which resets its statement and uses the
// connection. Without a `Drop` impl of its own, `Rows<'a>` would count for drop check only
// through its fields, which do not use `'a` when dropped, so the borrow could end before the drop:
// the statement or database could be dropped or replaced while rows that are never used again
// still exist, and `~Query` would then run on freed memory. This impl makes dropping the rows a
// use of `'a`. It must not be `#[may_dangle]`.
impl Drop for Rows<'_> {
    fn drop(&mut self) {}
}

fn bindings(params: &[Param<'_>]) -> Result<KjOwn<bridge::Bindings>> {
    let mut bindings = bridge::bindings_new(params.len());
    for (index, param) in params.iter().enumerate() {
        let target = bindings.as_mut();
        match *param {
            Param::Null => bridge::bindings_add_null(target),
            Param::Integer(value) => bridge::bindings_add_i64(target, value),
            Param::Real(value) => bridge::bindings_add_f64(target, value),
            Param::Text(value) => {
                check_param_length(index, value.len())?;
                bridge::bindings_add_text(target, value);
            }
            Param::Blob(value) => {
                check_param_length(index, value.len())?;
                bridge::bindings_add_blob(target, value);
            }
        }
    }
    Ok(bindings)
}

/// Fails for a text or blob parameter longer than an `int`, which SQLite takes its length as. A
/// shorter one longer than the database's `SQLITE_LIMIT_LENGTH` fails when SQLite binds it.
fn check_param_length(index: usize, len: usize) -> Result<()> {
    if i32::try_from(len).is_ok() {
        Ok(())
    } else {
        Err(KjError::new(
            KjExceptionType::Failed,
            format!(
                "params[{index}] is {len} bytes, more than SQLite takes ({} bytes)",
                i32::MAX
            ),
        ))
    }
}

#[cfg(test)]
#[path = "database-test.rs"]
mod tests;
