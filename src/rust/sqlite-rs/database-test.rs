// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::any::Any;
use std::path::Path;
use std::path::PathBuf;

use sqlite_rs_testing::checked;
use sqlite_rs_testing::temp_dir;

use super::*;

fn open_memory() -> Database {
    let vfs = Vfs::in_memory().unwrap();
    Database::open(&vfs, "test.sqlite", OpenMode::Create).unwrap()
}

fn column_names(db: &Database, table: &str) -> Vec<String> {
    let mut rows = db
        .run(&format!("PRAGMA table_info({table});"), &[])
        .unwrap();
    let mut names = Vec::new();
    while !rows.is_done() {
        // Column 1 of table_info is the column's name.
        names.push(rows.text(1).unwrap());
        rows.next_row().unwrap();
    }
    names
}

#[test]
#[expect(clippy::float_cmp, reason = "a stored value reads back exactly")]
fn params_round_trip() {
    checked(|| {
        let db = open_memory();
        // A column without a declared type keeps every value as it was bound.
        db.run("CREATE TABLE t (i INTEGER PRIMARY KEY, v);", &[])
            .unwrap();

        let none: Option<&str> = None;
        let params = [
            Param::Null,
            none.into(),
            i64::MIN.into(),
            i64::MAX.into(),
            1.5_f64.into(),
            "héllo".into(),
            "".into(),
            Some("named").into(),
            Param::Blob(&[0, 1, 255]),
            Param::Blob(&[]),
        ];
        let mut insert = db.prepare("INSERT INTO t (v) VALUES (?);").unwrap();
        for param in &params {
            insert.run(&[*param]).unwrap();
        }
        drop(insert);

        let mut rows = db
            .run("SELECT typeof(v), v FROM t ORDER BY i;", &[])
            .unwrap();
        let mut next = |expected_type: &str| {
            assert!(!rows.is_done());
            assert_eq!(rows.text(0).unwrap(), expected_type);
            let column = (
                rows.is_null(1).unwrap(),
                rows.maybe_blob(1).unwrap(),
                rows.i64(1).unwrap(),
                rows.f64(1).unwrap(),
            );
            let text = if expected_type == "blob" {
                None
            } else {
                Some(rows.maybe_text(1).unwrap())
            };
            rows.next_row().unwrap();
            (column, text)
        };

        for _ in 0..2 {
            assert_eq!(next("null"), ((true, None, 0, 0.0), Some(None)));
        }
        assert_eq!(next("integer").0.2, i64::MIN);
        assert_eq!(next("integer").0.2, i64::MAX);
        assert_eq!(next("real").0.3, 1.5);
        assert_eq!(next("text").1, Some(Some("héllo".to_owned())));
        // An empty string is text, not NULL.
        assert_eq!(
            next("text"),
            ((false, Some(vec![]), 0, 0.0), Some(Some(String::new())))
        );
        assert_eq!(next("text").1, Some(Some("named".to_owned())));
        assert_eq!(next("blob").0.1, Some(vec![0, 1, 255]));
        // So is an empty blob a blob.
        assert_eq!(next("blob").0, (false, Some(vec![]), 0, 0.0));
        assert!(rows.is_done());
    });
}

#[test]
fn several_params_of_one_statement() {
    checked(|| {
        let db = open_memory();
        let rows = db
            .run(
                "SELECT ? + ?, ? || ?, ?;",
                &[
                    2_i64.into(),
                    3_i64.into(),
                    "a".into(),
                    "b".into(),
                    Param::Null,
                ],
            )
            .unwrap();
        assert_eq!(rows.column_count().unwrap(), 3);
        assert_eq!(rows.i64(0).unwrap(), 5);
        assert_eq!(rows.text(1).unwrap(), "ab");
        assert_eq!(rows.maybe_text(2).unwrap(), None);
        // `text` of NULL is empty, as SQLite converts it.
        assert_eq!(rows.text(2).unwrap(), "");
    });
}

#[test]
fn change_count() {
    checked(|| {
        let db = open_memory();
        db.run(
            "CREATE TABLE t (k TEXT PRIMARY KEY, v INTEGER, name TEXT) WITHOUT ROWID;",
            &[],
        )
        .unwrap();

        // The alarm store's upsert: `name` changes only when one is given.
        let mut upsert = db
            .prepare(
                "INSERT INTO t VALUES (?, ?, ?)
                   ON CONFLICT DO UPDATE SET v = excluded.v,
                     name = COALESCE(excluded.name, t.name);",
            )
            .unwrap();
        let mut set = |key: &str, value: i64, name: Option<&str>| {
            let rows = upsert
                .run(&[key.into(), value.into(), name.into()])
                .unwrap();
            assert!(rows.is_done());
            rows.change_count().unwrap()
        };
        assert_eq!(set("a", 1, Some("first")), 1);
        assert_eq!(set("a", 2, None), 1);
        assert_eq!(set("b", 3, None), 1);

        let rows = db.run("SELECT v, name FROM t WHERE k = 'a';", &[]).unwrap();
        assert_eq!(rows.i64(0).unwrap(), 2);
        assert_eq!(rows.maybe_text(1).unwrap().as_deref(), Some("first"));
        drop(rows);

        let mut delete = db.prepare("DELETE FROM t WHERE k = ?;").unwrap();
        assert_eq!(
            delete.run(&["a".into()]).unwrap().change_count().unwrap(),
            1
        );
        assert_eq!(
            delete.run(&["a".into()]).unwrap().change_count().unwrap(),
            0
        );
        drop(delete);

        db.run("INSERT INTO t VALUES ('c', 4, NULL), ('d', 5, NULL);", &[])
            .unwrap();
        let rows = db.run("DELETE FROM t;", &[]).unwrap();
        assert_eq!(rows.change_count().unwrap(), 3);
        drop(rows);

        // A query that returns rows has no change count, even once it is done.
        let mut rows = db
            .run("INSERT INTO t VALUES ('e', 6, NULL) RETURNING k;", &[])
            .unwrap();
        assert_eq!(rows.text(0).unwrap(), "e");
        rows.next_row().unwrap();
        assert!(rows.is_done());
        let error = rows.change_count().err().unwrap();
        assert!(error.description().contains("returns rows"), "{error:?}");
        drop(rows);
        let mut rows = db.run("SELECT count(*) FROM t;", &[]).unwrap();
        rows.next_row().unwrap();
        assert!(rows.change_count().is_err());
    });
}

#[test]
fn change_count_is_zero_for_statements_that_change_no_rows() {
    checked(|| {
        let db = open_memory();
        db.run("CREATE TABLE t (v INTEGER);", &[]).unwrap();
        let count = |sql: &str| db.run(sql, &[]).unwrap().change_count().unwrap();
        // After each of these, SQLite's `sqlite3_changes()` is still the 2 of the INSERT.
        for sql in [
            "CREATE TABLE u (v INTEGER);",
            "PRAGMA user_version = 1;",
            "BEGIN;",
            "SAVEPOINT s;",
            "RELEASE s;",
            "COMMIT;",
            "DROP TABLE u;",
        ] {
            assert_eq!(count("INSERT INTO t VALUES (1), (2);"), 2);
            assert_eq!(count(sql), 0, "{sql}");
        }
        assert_eq!(count("UPDATE t SET v = 3 WHERE v = 0;"), 0);
        // The prelude of a run of several statements counts for none of them.
        assert_eq!(count("DELETE FROM t; CREATE TABLE u (v INTEGER);"), 0);
        assert_eq!(count("DELETE FROM t;"), 0);
    });
}

#[test]
fn change_count_is_the_statements_own() {
    checked(|| {
        let db = open_memory();
        db.run("CREATE TABLE t (v INTEGER);", &[]).unwrap();
        let first = db.run("INSERT INTO t VALUES (1), (2), (3);", &[]).unwrap();
        let second = db.run("DELETE FROM t WHERE v = 1;", &[]).unwrap();
        assert_eq!(first.change_count().unwrap(), 3);
        assert_eq!(second.change_count().unwrap(), 1);
    });
}

#[test]
fn finish_returns_the_error_that_dropping_discards() {
    // Outside a transaction, the deferred foreign key is checked when the statement commits, after
    // its last row.
    const INSERT: &str = "INSERT INTO child VALUES (1), (2) RETURNING parent_id;";
    checked(|| {
        let db = open_memory();
        db.run(
            "PRAGMA foreign_keys = ON;
             CREATE TABLE parent (id INTEGER PRIMARY KEY);
             CREATE TABLE child (
               parent_id INTEGER REFERENCES parent (id) DEFERRABLE INITIALLY DEFERRED
             );",
            &[],
        )
        .unwrap();
        let count = || {
            db.run("SELECT count(*) FROM child;", &[])
                .unwrap()
                .i64(0)
                .unwrap()
        };

        let rows = db.run(INSERT, &[]).unwrap();
        assert_eq!(rows.i64(0).unwrap(), 1);
        drop(rows);
        // The commit failed, and the insert was rolled back, without an error.
        assert_eq!(count(), 0);

        let rows = db.run(INSERT, &[]).unwrap();
        assert_eq!(rows.i64(0).unwrap(), 1);
        let error = rows.finish().err().unwrap();
        assert!(
            error
                .description()
                .contains("FOREIGN KEY constraint failed"),
            "{error:?}"
        );
        assert_eq!(count(), 0);

        db.run("INSERT INTO parent VALUES (1), (2);", &[]).unwrap();
        db.run(INSERT, &[]).unwrap().finish().unwrap();
        assert_eq!(count(), 2);
        // A query that is already done finishes at once.
        db.run("DELETE FROM child;", &[]).unwrap().finish().unwrap();
    });
}

#[test]
fn text_binds_to_a_utf16_database() {
    checked(|| {
        let db = open_memory();
        // SQLite converts bound text to the database's encoding when it binds it, which the
        // metering allocator must see inside the database's memory scope.
        db.run("PRAGMA encoding = 'UTF-16';", &[]).unwrap();
        db.run("CREATE TABLE t (v TEXT);", &[]).unwrap();
        assert_eq!(
            db.run("PRAGMA encoding;", &[]).unwrap().text(0).unwrap(),
            "UTF-16le"
        );
        let text = "héllo".repeat(1000);
        let mut insert = db.prepare("INSERT INTO t VALUES (?);").unwrap();
        for _ in 0..100 {
            insert.run(&[text.as_str().into()]).unwrap();
        }
        drop(insert);
        let rows = db
            .run("SELECT count(*), min(v) = max(v), max(v) FROM t;", &[])
            .unwrap();
        assert_eq!(rows.i64(0).unwrap(), 100);
        assert_eq!(rows.i64(1).unwrap(), 1);
        assert_eq!(rows.text(2).unwrap(), text);
    });
}

#[test]
fn paths_with_a_backslash_or_a_colon_fail() {
    checked(|| {
        let (dir, vfs) = on_disk("paths");
        for path in [
            "..\\escaped.sqlite",
            "sub\\..\\..\\escaped.sqlite",
            "sub/a\\b.sqlite",
            "C:escaped.sqlite",
            "sub/c:.sqlite",
            "x.sqlite:stream",
        ] {
            let error = Database::open(&vfs, path, OpenMode::CreateWithParents)
                .err()
                .unwrap_or_else(|| panic!("{path} opened"));
            assert!(
                error.description().contains("cannot contain '\\' or ':'"),
                "{path}: {error:?}"
            );
        }
        assert!(std::fs::read_dir(&dir).unwrap().next().is_none());
        Database::open(&vfs, "sub/./db.sqlite", OpenMode::CreateWithParents).unwrap();
        assert!(dir.join("sub").join("db.sqlite").exists());
    });
}

#[test]
fn params_longer_than_an_int_fail() {
    let max = usize::try_from(i32::MAX).unwrap();
    assert!(check_param_length(0, max).is_ok());
    let error = check_param_length(3, max + 1).err().unwrap();
    assert!(
        error
            .description()
            .contains("params[3] is 2147483648 bytes"),
        "{error:?}"
    );
}

#[test]
fn params_longer_than_the_length_limit_fail() {
    checked(|| {
        let db = open_memory();
        // `SqliteDatabase` limits values to `MAX_ROW_LENGTH`, 8 MiB and 34 bytes.
        let long = vec![b'x'; (8 << 20) + 35];
        let text = std::str::from_utf8(&long).unwrap();
        for param in [Param::Blob(&long), Param::Text(text)] {
            let error = db.run("SELECT length(?);", &[param]).err().unwrap();
            assert!(error.description().contains("too big"), "{error:?}");
        }
        let fits = &long[..8 << 20];
        let rows = db.run("SELECT length(?);", &[Param::Blob(fits)]).unwrap();
        assert_eq!(rows.i64(0).unwrap(), 8 << 20);
    });
}

#[test]
fn alarm_store_schema_migration() {
    checked(|| {
        let vfs = Vfs::in_memory().unwrap();
        // A database from before the `actor_name` column.
        {
            let db = Database::open(&vfs, "metadata.sqlite", OpenMode::CreateWithParents).unwrap();
            db.run(
                "CREATE TABLE _cf_ALARM (
                   actor_id TEXT PRIMARY KEY,
                   scheduled_time INTEGER
                 ) WITHOUT ROWID;
                 INSERT INTO _cf_ALARM VALUES ('old', 7);",
                &[],
            )
            .unwrap();
        }

        // What the alarm store does on opening, twice: once to migrate, once to find it migrated.
        for migrates in [true, false] {
            let db = Database::open(&vfs, "metadata.sqlite", OpenMode::CreateWithParents).unwrap();
            let mode = db.run("PRAGMA journal_mode=WAL;", &[]).unwrap();
            assert_eq!(mode.text(0).unwrap(), "wal");
            drop(mode);

            db.run(
                "CREATE TABLE IF NOT EXISTS _cf_ALARM (
                   actor_id TEXT PRIMARY KEY,
                   scheduled_time INTEGER,
                   actor_name TEXT
                 ) WITHOUT ROWID;",
                &[],
            )
            .unwrap();

            let has_name = column_names(&db, "_cf_ALARM").contains(&"actor_name".to_owned());
            assert_eq!(has_name, !migrates);
            if !has_name {
                db.run("ALTER TABLE _cf_ALARM ADD COLUMN actor_name TEXT;", &[])
                    .unwrap();
            }
            assert_eq!(
                column_names(&db, "_cf_ALARM"),
                ["actor_id", "scheduled_time", "actor_name"]
            );

            let rows = db
                .run(
                    "SELECT actor_id, scheduled_time, actor_name FROM _cf_ALARM;",
                    &[],
                )
                .unwrap();
            assert_eq!(rows.text(0).unwrap(), "old");
            assert_eq!(rows.i64(1).unwrap(), 7);
            assert_eq!(rows.maybe_text(2).unwrap(), None);
        }
    });
}

#[test]
fn errors() {
    checked(|| {
        let db = open_memory();

        let error = db.run("SELEC 1;", &[]).err().unwrap();
        assert!(error.description().contains("syntax error"), "{error:?}");

        let error = db.run("SELECT ?;", &[]).err().unwrap();
        assert!(
            error
                .description()
                .contains("Wrong number of parameter bindings"),
            "{error:?}"
        );

        let error = db.prepare("SELECT 1; SELECT 2;").err().unwrap();
        assert!(
            error.description().contains("only one statement"),
            "{error:?}"
        );

        let mut rows = db.run("SELECT 1;", &[]).unwrap();
        assert!(rows.i64(1).is_err());
        assert!(rows.change_count().is_err());
        rows.next_row().unwrap();
        assert!(rows.is_done());
        assert!(rows.i64(0).is_err());
        assert!(rows.next_row().is_err());
        drop(rows);

        let rows = db.run("SELECT CAST(x'ff' AS TEXT);", &[]).unwrap();
        let error = rows.text(0).err().unwrap();
        assert!(error.description().contains("not UTF-8"), "{error:?}");
        assert_eq!(rows.blob(0).unwrap(), [0xff]);
    });
}

#[test]
fn text_with_an_embedded_nul() {
    checked(|| {
        let db = open_memory();
        let rows = db
            .run("SELECT ?, hex(?1), typeof(?1);", &["a\0b".into()])
            .unwrap();
        let text = rows.text(0).unwrap();
        assert_eq!(text.len(), 3);
        assert_eq!(text, "a\0b");
        assert_eq!(rows.text(1).unwrap(), "610062");
        assert_eq!(rows.text(2).unwrap(), "text");
    });
}

#[test]
fn two_database_queries_interleave() {
    checked(|| {
        let db = open_memory();
        db.run(
            "CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1), (2), (3);",
            &[],
        )
        .unwrap();
        let mut ascending = db.run("SELECT v FROM t ORDER BY v;", &[]).unwrap();
        let mut descending = db.run("SELECT v FROM t ORDER BY v DESC;", &[]).unwrap();
        let mut seen = Vec::new();
        while !ascending.is_done() {
            seen.push((ascending.i64(0).unwrap(), descending.i64(0).unwrap()));
            ascending.next_row().unwrap();
            descending.next_row().unwrap();
        }
        assert!(descending.is_done());
        assert_eq!(seen, [(1, 3), (2, 2), (3, 1)]);
    });
}

#[test]
fn database_query_interleaves_with_a_statement() {
    checked(|| {
        let db = open_memory();
        db.run(
            "CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1), (2);",
            &[],
        )
        .unwrap();
        db.run("CREATE TABLE u (v INTEGER);", &[]).unwrap();
        let mut insert = db.prepare("INSERT INTO u VALUES (?);").unwrap();

        // Copy t into u, row by row, while the query over t is running.
        let mut rows = db.run("SELECT v FROM t ORDER BY v;", &[]).unwrap();
        while !rows.is_done() {
            let value = rows.i64(0).unwrap();
            let inserted = insert.run(&[(value * 10).into()]).unwrap();
            assert_eq!(inserted.change_count().unwrap(), 1);
            drop(inserted);
            rows.next_row().unwrap();
        }
        drop(rows);
        let rows = db.run("SELECT group_concat(v) FROM u;", &[]).unwrap();
        assert_eq!(rows.text(0).unwrap(), "10,20");
    });
}

/// Rows whose second step fails: `abs()` of the smallest integer overflows.
fn rows_failing_on_second_step(db: &Database) -> Rows<'_> {
    db.run(
        "CREATE TABLE t (i INTEGER PRIMARY KEY, v INTEGER);
         INSERT INTO t (v) VALUES (-1), (?), (-3);",
        &[i64::MIN.into()],
    )
    .unwrap();
    let mut rows = db.run("SELECT abs(v) FROM t ORDER BY i;", &[]).unwrap();
    assert_eq!(rows.i64(0).unwrap(), 1);
    let error = rows.next_row().err().unwrap();
    assert!(
        error.description().contains("integer overflow"),
        "{error:?}"
    );
    rows
}

#[test]
fn failed_step_does_not_restart_the_query() {
    checked(|| {
        let db = open_memory();
        let mut rows = rows_failing_on_second_step(&db);
        // SQLite would reset the statement and run it again from the first row.
        let error = rows.next_row().err().unwrap();
        assert!(error.description().contains("failed"), "{error:?}");
        assert!(!rows.is_done());
    });
}

#[test]
fn getters_fail_after_a_failed_step() {
    checked(|| {
        let db = open_memory();
        let rows = rows_failing_on_second_step(&db);
        assert!(rows.i64(0).is_err());
        assert!(rows.is_null(0).is_err());
        assert!(rows.text(0).is_err());
        assert!(rows.change_count().is_err());
        drop(rows);
        // The connection is still usable.
        assert_eq!(
            db.run("SELECT count(*) FROM t;", &[])
                .unwrap()
                .i64(0)
                .unwrap(),
            3
        );
    });
}

#[test]
fn two_statement_queries_interleave() {
    checked(|| {
        let db = open_memory();
        db.run(
            "CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1), (2);",
            &[],
        )
        .unwrap();
        let mut first = db.prepare("SELECT v FROM t ORDER BY v;").unwrap();
        let mut second = db.prepare("SELECT count(*) FROM t;").unwrap();
        let mut rows = first.run(&[]).unwrap();
        let count = second.run(&[]).unwrap();
        assert_eq!(rows.i64(0).unwrap(), 1);
        assert_eq!(count.i64(0).unwrap(), 2);
        rows.next_row().unwrap();
        assert_eq!(rows.i64(0).unwrap(), 2);
    });
}

// Statements don't outlive their database: a statement holds the connection open, and the
// connection is closed once, after the last handle and statement, whatever the order.
//
// Two observations check it. In WAL mode, closing the last connection to a file checkpoints the
// WAL into it and deletes `db.sqlite-wal` (an on-disk VFS uses SQLite's native VFS, whose files
// are the real ones), so the WAL shows when SQLite closed the connection. `~SqliteDatabase` logs
// "sqlite database destroyed while dependent objects still exist" at ERROR when statements
// outlive it, which `checked` turns into a failure; this is what catches a premature close, since
// SQLite then keeps the connection as a zombie until its last statement is finalized, and the WAL
// goes at the same moment either way.

const DB: &str = "db.sqlite";

/// A new on-disk directory for `name` and a VFS over it.
fn on_disk(name: &str) -> (PathBuf, Vfs) {
    let dir = temp_dir(name);
    let vfs = Vfs::on_disk(dir.to_str().unwrap()).unwrap();
    (dir, vfs)
}

/// Opens `DB` in WAL mode, with the table `t` and the row `kept` written to the WAL.
fn open_wal(vfs: &Vfs) -> Database {
    let db = Database::open(vfs, DB, OpenMode::Create).unwrap();
    assert_eq!(
        db.run("PRAGMA journal_mode=WAL;", &[])
            .unwrap()
            .text(0)
            .unwrap(),
        "wal"
    );
    db.run(
        "CREATE TABLE IF NOT EXISTS t (k TEXT PRIMARY KEY, v INTEGER) WITHOUT ROWID;
         INSERT OR REPLACE INTO t VALUES ('kept', 0);",
        &[],
    )
    .unwrap();
    db
}

fn wal_exists(dir: &Path) -> bool {
    dir.join(format!("{DB}-wal")).exists()
}

/// The rows of `t`, read through a new VFS and connection once every other one is closed.
fn rows_after_reopening(dir: &Path) -> Vec<(String, i64)> {
    assert!(!wal_exists(dir), "a connection is still open");
    let vfs = Vfs::on_disk(dir.to_str().unwrap()).unwrap();
    let db = Database::open(&vfs, DB, OpenMode::ReadWrite).unwrap();
    let mut rows = db.run("SELECT k, v FROM t ORDER BY k;", &[]).unwrap();
    let mut result = Vec::new();
    while !rows.is_done() {
        result.push((rows.text(0).unwrap(), rows.i64(1).unwrap()));
        rows.next_row().unwrap();
    }
    result
}

fn pairs(expected: &[(&str, i64)]) -> Vec<(String, i64)> {
    expected
        .iter()
        .map(|(k, v)| ((*k).to_owned(), *v))
        .collect()
}

const SET: &str = "INSERT INTO t VALUES (?, ?) ON CONFLICT DO UPDATE SET v = excluded.v;";
const GET: &str = "SELECT v FROM t WHERE k = ?;";
const REMOVE: &str = "DELETE FROM t WHERE k = ?;";

fn set(statement: &mut Statement, key: &str, value: i64) {
    let rows = statement.run(&[key.into(), value.into()]).unwrap();
    assert_eq!(rows.change_count().unwrap(), 1);
}

fn get(statement: &mut Statement, key: &str) -> Option<i64> {
    let rows = statement.run(&[key.into()]).unwrap();
    if rows.is_done() {
        None
    } else {
        Some(rows.i64(0).unwrap())
    }
}

#[test]
fn statement_outlives_database_handle() {
    checked(|| {
        let (dir, vfs) = on_disk("outlives");
        let db = open_wal(&vfs);
        let mut setter = db.prepare(SET).unwrap();
        let mut getter = db.prepare(GET).unwrap();
        drop(db);
        drop(vfs);

        set(&mut setter, "a", 1);
        assert_eq!(get(&mut getter, "a"), Some(1));
        assert_eq!(get(&mut getter, "kept"), Some(0));
        // The statement's database is a handle like any other.
        setter
            .database()
            .run("INSERT INTO t VALUES ('b', 2);", &[])
            .unwrap();
        assert_eq!(get(&mut getter, "b"), Some(2));
        assert!(wal_exists(&dir));

        drop(setter);
        drop(getter);
        assert_eq!(
            rows_after_reopening(&dir),
            pairs(&[("a", 1), ("b", 2), ("kept", 0)])
        );
    });
}

#[test]
fn database_closes_after_last_statement() {
    checked(|| {
        let (dir, vfs) = on_disk("closes");
        let db = open_wal(&vfs);
        let first = db.prepare(GET).unwrap();
        let second = db.prepare(GET).unwrap();
        assert!(wal_exists(&dir));

        drop(db);
        assert!(wal_exists(&dir), "closed with statements left");
        drop(first);
        assert!(wal_exists(&dir), "closed with a statement left");
        drop(second);
        assert!(!wal_exists(&dir), "not closed after the last statement");
        assert_eq!(rows_after_reopening(&dir), pairs(&[("kept", 0)]));
    });
}

// A store keeps its database and statements side by side; the two declaration orders drop them in
// opposite orders.
struct DatabaseFirst {
    db: Database,
    set: Statement,
    remove: Statement,
}

struct StatementsFirst {
    set: Statement,
    remove: Statement,
    db: Database,
}

#[test]
fn store_field_order_does_not_matter() {
    checked(|| {
        let (dir, vfs) = on_disk("database-first");
        let db = open_wal(&vfs);
        let mut store = DatabaseFirst {
            set: db.prepare(SET).unwrap(),
            remove: db.prepare(REMOVE).unwrap(),
            db,
        };
        set(&mut store.set, "a", 1);
        store.remove.run(&["kept".into()]).unwrap();
        store.db.run("INSERT INTO t VALUES ('b', 2);", &[]).unwrap();
        drop(store);
        assert_eq!(rows_after_reopening(&dir), pairs(&[("a", 1), ("b", 2)]));

        let (dir, vfs) = on_disk("statements-first");
        let db = open_wal(&vfs);
        let mut store = StatementsFirst {
            set: db.prepare(SET).unwrap(),
            remove: db.prepare(REMOVE).unwrap(),
            db,
        };
        set(&mut store.set, "a", 1);
        store.remove.run(&["kept".into()]).unwrap();
        store.db.run("INSERT INTO t VALUES ('b', 2);", &[]).unwrap();
        drop(store);
        assert_eq!(rows_after_reopening(&dir), pairs(&[("a", 1), ("b", 2)]));
    });
}

#[test]
fn drop_order_does_not_matter() {
    // Every order of dropping the handle and two statements: the handle first, in the middle and
    // last, and the statements either way round.
    let orders = [
        [0, 1, 2],
        [0, 2, 1],
        [1, 0, 2],
        [2, 0, 1],
        [1, 2, 0],
        [2, 1, 0],
    ];
    checked(|| {
        for (i, order) in orders.iter().enumerate() {
            let (dir, vfs) = on_disk(&format!("drop-order-{i}"));
            let db = open_wal(&vfs);
            let mut setter = db.prepare(SET).unwrap();
            let mut remover = db.prepare(REMOVE).unwrap();
            set(&mut setter, "a", 1);
            remover.run(&["kept".into()]).unwrap();
            drop(vfs);

            let mut handles: Vec<Option<Box<dyn Any>>> = vec![
                Some(Box::new(db)),
                Some(Box::new(setter)),
                Some(Box::new(remover)),
            ];
            for (n, &index) in order.iter().enumerate() {
                assert!(wal_exists(&dir), "order {order:?}: closed before drop {n}");
                handles[index] = None;
            }
            assert!(!wal_exists(&dir), "order {order:?}: not closed");
            assert_eq!(rows_after_reopening(&dir), pairs(&[("a", 1)]), "{order:?}");
        }
    });
}

#[test]
fn statement_from_a_clone() {
    checked(|| {
        let (dir, vfs) = on_disk("clone");
        let db = open_wal(&vfs);
        let clone = db.clone();
        let mut setter = clone.prepare(SET).unwrap();
        let mut getter = db.prepare(GET).unwrap();
        drop(db);
        drop(clone);

        set(&mut setter, "a", 1);
        assert_eq!(get(&mut getter, "a"), Some(1));
        drop(getter);
        assert!(wal_exists(&dir));
        drop(setter);
        assert_eq!(rows_after_reopening(&dir), pairs(&[("a", 1), ("kept", 0)]));
    });
}
