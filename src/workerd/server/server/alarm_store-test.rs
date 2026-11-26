// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::path::PathBuf;
use std::time::Duration;
use std::time::SystemTime;

use sqlite_rs::Database;
use sqlite_rs::OpenMode;
use sqlite_rs::Vfs;

use super::AlarmStore;
use crate::actor_alarms::time_to_ns;
use crate::alarm_scheduler::ActorKey;
use crate::alarm_scheduler::AlarmStorage;
use crate::alarm_scheduler::StoredAlarm;

const PATH: &str = "alarms";
const HOUR: Duration = Duration::from_hours(1);

/// Historically the alarm scheduler was C++ (`AlarmScheduler`), with the statements that the
/// store runs, so the store reads and writes the databases it left as they are.
///
/// A `metadata.sqlite` that the C++ scheduler wrote, in a one-off test, holding two alarms: actor
/// `6ba7b8109dad11d180b400c04fd430c8`'s, named `my-name`, at 1764000000123456789 ns, and actor
/// `0123456789abcdef0123456789abcdef`'s, nameless, an hour less a nanosecond before the epoch.
/// The scheduler closed it, so its WAL was checkpointed into it and removed.
const CPP_SCHEDULER_DATABASE: &[u8] = include_bytes!("testdata/c++-alarm-scheduler.sqlite");

/// The `_cf_ALARM` table's `CREATE TABLE` as SQLite keeps it in a database that the C++ scheduler
/// created.
const CPP_SCHEDULER_SCHEMA: &str = "CREATE TABLE _cf_ALARM (\n      actor_id TEXT PRIMARY KEY,\n      \
                                    scheduled_time INTEGER,\n      actor_name TEXT\n    ) WITHOUT ROWID";

fn schema(db: &Database) -> String {
    db.run(
        "SELECT sql FROM sqlite_schema WHERE name = '_cf_ALARM';",
        &[],
    )
    .unwrap()
    .text(0)
    .unwrap()
}

fn t0() -> SystemTime {
    SystemTime::UNIX_EPOCH + HOUR
}

fn key(id: &str, name: Option<&str>) -> ActorKey {
    ActorKey {
        id: id.to_owned(),
        name: name.map(str::to_owned),
    }
}

fn open(vfs: &Vfs) -> AlarmStore {
    AlarmStore::open(vfs, PATH).unwrap()
}

fn sorted(mut alarms: Vec<StoredAlarm>) -> Vec<StoredAlarm> {
    alarms.sort_by(|a, b| a.actor.id.cmp(&b.actor.id));
    alarms
}

/// A new, empty directory for `name` under the test's temporary directory.
fn temp_dir(name: &str) -> PathBuf {
    let root = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let dir = root.join(format!("alarm-store-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn starts_empty() {
    let vfs = Vfs::in_memory().unwrap();
    assert!(open(&vfs).load().unwrap().is_empty());
}

#[test]
fn loads_what_it_stored_across_reopening() {
    let vfs = Vfs::in_memory().unwrap();
    {
        let store = open(&vfs);
        assert!(store.set(&key("named", Some("my-name")), t0()).unwrap());
        assert!(store.set(&key("nameless", None), t0() + HOUR).unwrap());
    }

    assert_eq!(
        sorted(open(&vfs).load().unwrap()),
        [
            StoredAlarm {
                actor: key("named", Some("my-name")),
                scheduled_time: t0(),
            },
            StoredAlarm {
                actor: key("nameless", None),
                scheduled_time: t0() + HOUR,
            },
        ]
    );
}

#[test]
fn keeps_a_stored_name_when_an_alarm_is_set_again_without_one() {
    let vfs = Vfs::in_memory().unwrap();
    let store = open(&vfs);
    assert!(store.set(&key("actor", Some("my-name")), t0()).unwrap());
    assert!(store.set(&key("actor", None), t0() + HOUR).unwrap());
    assert!(
        store
            .set(&key("actor", Some("new-name")), t0() + 2 * HOUR)
            .unwrap()
    );
    assert!(store.set(&key("actor", None), t0() + 3 * HOUR).unwrap());

    assert_eq!(
        store.load().unwrap(),
        [StoredAlarm {
            actor: key("actor", Some("new-name")),
            scheduled_time: t0() + 3 * HOUR,
        }]
    );
}

#[test]
fn stores_times_before_the_epoch_and_to_the_nanosecond() {
    let vfs = Vfs::in_memory().unwrap();
    let store = open(&vfs);
    let early = SystemTime::UNIX_EPOCH - HOUR + Duration::from_nanos(1);
    store.set(&key("actor", None), early).unwrap();

    let alarms = store.load().unwrap();
    assert_eq!(alarms.len(), 1);
    assert_eq!(alarms[0].scheduled_time, early);
}

#[test]
fn reports_whether_remove_removed_an_alarm() {
    let vfs = Vfs::in_memory().unwrap();
    let store = open(&vfs);
    assert!(!store.remove("actor").unwrap());
    store.set(&key("actor", None), t0()).unwrap();
    assert!(store.remove("actor").unwrap());
    assert!(!store.remove("actor").unwrap());
    assert!(store.load().unwrap().is_empty());
}

#[test]
fn clear_removes_every_alarm() {
    let vfs = Vfs::in_memory().unwrap();
    {
        let store = open(&vfs);
        store.set(&key("a", None), t0()).unwrap();
        store.set(&key("b", Some("name")), t0()).unwrap();
        store.clear().unwrap();
        assert!(store.load().unwrap().is_empty());
    }
    assert!(open(&vfs).load().unwrap().is_empty());
}

#[test]
fn migrates_a_database_created_before_the_actor_name_column_existed() {
    let vfs = Vfs::in_memory().unwrap();

    // The schema of a database persisted by an older workerd, which lacks `actor_name`, with one
    // pending alarm.
    {
        let db = Database::open(&vfs, PATH, OpenMode::Create).unwrap();
        drop(db.run("PRAGMA journal_mode=WAL;", &[]).unwrap());
        db.run(
            "
            CREATE TABLE _cf_ALARM (
              actor_id TEXT PRIMARY KEY,
              scheduled_time INTEGER
            ) WITHOUT ROWID;
            ",
            &[],
        )
        .unwrap();
        db.run(
            "INSERT INTO _cf_ALARM VALUES (?, ?)",
            &["old-actor".into(), time_to_ns(t0()).into()],
        )
        .unwrap();
    }

    {
        let store = open(&vfs);
        assert_eq!(
            store.load().unwrap(),
            [StoredAlarm {
                actor: key("old-actor", None),
                scheduled_time: t0(),
            }]
        );
        // The migrated table takes names.
        store.set(&key("old-actor", Some("my-name")), t0()).unwrap();
    }

    {
        let db = Database::open(&vfs, PATH, OpenMode::ReadWrite).unwrap();
        let query = db
            .run(
                "SELECT actor_name FROM _cf_ALARM WHERE actor_id = ?",
                &["old-actor".into()],
            )
            .unwrap();
        assert!(!query.is_done());
        assert_eq!(query.maybe_text(0).unwrap().as_deref(), Some("my-name"));
    }

    // Opening a migrated database again leaves it as it is.
    let alarms = open(&vfs).load().unwrap();
    assert_eq!(alarms.len(), 1);
    assert_eq!(alarms[0].actor.name.as_deref(), Some("my-name"));
}

#[test]
fn opens_a_database_that_the_cpp_scheduler_wrote() {
    let dir = temp_dir("cpp");
    std::fs::write(dir.join("metadata.sqlite"), CPP_SCHEDULER_DATABASE).unwrap();
    let vfs = Vfs::on_disk(dir.to_str().unwrap()).unwrap();
    let named = key("6ba7b8109dad11d180b400c04fd430c8", Some("my-name"));
    let nameless = key("0123456789abcdef0123456789abcdef", None);
    let early = SystemTime::UNIX_EPOCH - HOUR + Duration::from_nanos(1);
    {
        let store = AlarmStore::open(&vfs, "metadata.sqlite").unwrap();
        assert_eq!(
            sorted(store.load().unwrap()),
            [
                StoredAlarm {
                    actor: nameless.clone(),
                    scheduled_time: early,
                },
                StoredAlarm {
                    actor: named.clone(),
                    scheduled_time: SystemTime::UNIX_EPOCH
                        + Duration::from_nanos(1_764_000_000_123_456_789),
                },
            ]
        );

        // The rows go on as the C++ scheduler's would: the name stays, the time changes.
        assert!(store.set(&key(&named.id, None), t0()).unwrap());
        assert!(store.remove(&nameless.id).unwrap());
    }

    let db = Database::open(&vfs, "metadata.sqlite", OpenMode::ReadOnly).unwrap();
    let rows = db
        .run(
            "SELECT actor_id, scheduled_time, actor_name FROM _cf_ALARM;",
            &[],
        )
        .unwrap();
    assert_eq!(rows.text(0).unwrap(), named.id);
    assert_eq!(rows.i64(1).unwrap(), time_to_ns(t0()));
    assert_eq!(rows.maybe_text(2).unwrap().as_deref(), Some("my-name"));
    let mut rows = rows;
    rows.next_row().unwrap();
    assert!(rows.is_done());
    drop(rows);
    // The database is still in WAL mode, and its schema is the C++ scheduler's.
    let mode = db.run("PRAGMA journal_mode;", &[]).unwrap();
    assert_eq!(mode.text(0).unwrap(), "wal");
    drop(mode);
    assert_eq!(schema(&db), CPP_SCHEDULER_SCHEMA);
}

#[test]
fn creates_the_table_as_the_cpp_scheduler_does() {
    let vfs = Vfs::in_memory().unwrap();
    let store = open(&vfs);
    assert_eq!(schema(&store.db), CPP_SCHEDULER_SCHEMA);
}
