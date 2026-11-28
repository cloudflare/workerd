// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Dropping rows destroys the C++ query, which uses its statement and connection, so the rows'
// borrow lasts until they are dropped, even with no later use of them.
use sqlite_rs::Database;
use sqlite_rs::Result;
use sqlite_rs::Statement;
use sqlite_rs::Vfs;

pub fn database_dropped(db: Database) -> Result<()> {
    let _rows = db.run("SELECT 1;", &[])?;
    drop(db); //~ ERROR E0505
    Ok(())
}

pub fn database_reassigned(vfs: &Vfs, mut db: Database) -> Result<()> {
    let _rows = db.run("SELECT 1;", &[])?;
    db = Database::open(vfs, "other.sqlite", sqlite_rs::OpenMode::Create)?; //~ ERROR E0506
    drop(db);
    Ok(())
}

pub fn statement_dropped(mut statement: Statement) -> Result<()> {
    let _rows = statement.run(&[])?;
    drop(statement); //~ ERROR E0505
    Ok(())
}

pub fn statement_reassigned(db: &Database) -> Result<()> {
    let mut statement = db.prepare("SELECT 1;")?;
    let _rows = statement.run(&[])?;
    statement = db.prepare("SELECT 2;")?; //~ ERROR E0506
    drop(statement);
    Ok(())
}
