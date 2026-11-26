// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The persisted alarms of a Durable Object namespace.

use std::cell::RefCell;
use std::time::SystemTime;

use sqlite_rs::Database;
use sqlite_rs::OpenMode;
use sqlite_rs::Statement;
use sqlite_rs::Vfs;

use crate::Result;
use crate::actor_alarms::time_from_ns;
use crate::actor_alarms::time_to_ns;
use crate::alarm_scheduler::ActorKey;
use crate::alarm_scheduler::AlarmStorage;
use crate::alarm_scheduler::StoredAlarm;

/// SQLite keeps the text of `CREATE TABLE` in the database (`sqlite_schema.sql`), so this is
/// spaced exactly as the databases that exist already have it.
const CREATE_TABLE: &str = "
    CREATE TABLE IF NOT EXISTS _cf_ALARM (
      actor_id TEXT PRIMARY KEY,
      scheduled_time INTEGER,
      actor_name TEXT
    ) WITHOUT ROWID;
  ";

/// A namespace's persisted alarms.
///
/// They are one row per root actor in the `_cf_ALARM` table of a SQLite database of their own.
/// The scheduler keeps every alarm in memory as well, and reads the table only at startup. An
/// alarm's time is stored as nanoseconds since the Unix epoch, negative before it.
pub struct AlarmStore {
    // The statements hold the database, so the order of the fields does not matter.
    set: RefCell<Statement>,
    remove: RefCell<Statement>,
    db: Database,
}

impl AlarmStore {
    /// Opens the database at `path` in `vfs`, creating it and its parent directories, or adding
    /// the `actor_name` column that databases older than the column lack.
    pub fn open(vfs: &Vfs, path: &str) -> Result<Self> {
        let db = Database::open(vfs, path, OpenMode::CreateWithParents)?;

        drop(db.run("PRAGMA journal_mode=WAL;", &[])?);

        drop(db.run(CREATE_TABLE, &[])?);

        // Migrate databases created before the `actor_name` column existed. SQLite has no
        // "ADD COLUMN IF NOT EXISTS", so detect the column via PRAGMA table_info first.
        let mut has_name_column = false;
        {
            let mut info = db.run("PRAGMA table_info(_cf_ALARM);", &[])?;
            while !info.is_done() {
                // Column 1 of table_info output is the column name.
                if info.text(1)? == "actor_name" {
                    has_name_column = true;
                    break;
                }
                info.next_row()?;
            }
        }
        if !has_name_column {
            drop(db.run("ALTER TABLE _cf_ALARM ADD COLUMN actor_name TEXT;", &[])?);
        }

        // On upsert we always refresh scheduled_time, but only overwrite actor_name when the
        // incoming row actually carries one. COALESCE keeps the previously-stored name when the
        // incoming one is NULL instead of clobbering it.
        let set = db.prepare(
            "
            INSERT INTO _cf_ALARM VALUES(?, ?, ?)
              ON CONFLICT DO UPDATE SET scheduled_time = excluded.scheduled_time,
                actor_name = COALESCE(excluded.actor_name, _cf_ALARM.actor_name);
            ",
        )?;
        let remove = db.prepare(
            "
            DELETE FROM _cf_ALARM WHERE actor_id = ?
            ",
        )?;

        Ok(Self {
            set: RefCell::new(set),
            remove: RefCell::new(remove),
            db,
        })
    }
}

impl AlarmStorage for AlarmStore {
    fn load(&self) -> Result<Vec<StoredAlarm>> {
        let mut alarms = Vec::new();
        let mut query = self.db.run(
            "
            SELECT actor_id, scheduled_time, actor_name FROM _cf_ALARM;
            ",
            &[],
        )?;
        while !query.is_done() {
            alarms.push(StoredAlarm {
                actor: ActorKey {
                    id: query.text(0)?,
                    name: query.maybe_text(2)?,
                },
                scheduled_time: time_from_ns(query.i64(1)?),
            });
            query.next_row()?;
        }
        Ok(alarms)
    }

    fn set(&self, actor: &ActorKey, scheduled_time: SystemTime) -> Result<bool> {
        let mut set = self.set.borrow_mut();
        let rows = set.run(&[
            actor.id.as_str().into(),
            time_to_ns(scheduled_time).into(),
            actor.name.as_deref().into(),
        ])?;
        Ok(rows.change_count()? > 0)
    }

    fn remove(&self, id: &str) -> Result<bool> {
        let mut remove = self.remove.borrow_mut();
        let rows = remove.run(&[id.into()])?;
        Ok(rows.change_count()? > 0)
    }

    fn clear(&self) -> Result<()> {
        drop(self.db.run("DELETE FROM _cf_ALARM;", &[])?);
        Ok(())
    }
}

#[cfg(test)]
#[path = "alarm_store-test.rs"]
mod tests;
