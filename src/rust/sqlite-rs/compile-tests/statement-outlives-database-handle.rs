// A statement holds its database, so it may outlive the handle it was prepared from, and a
// store may keep both side by side, in either order.
use sqlite_rs::Database;
use sqlite_rs::OpenMode;
use sqlite_rs::Result;
use sqlite_rs::Statement;
use sqlite_rs::Vfs;

pub fn statement(vfs: &Vfs) -> Result<Statement> {
    let db = Database::open(vfs, "db.sqlite", OpenMode::Create)?;
    db.prepare("SELECT 1;")
}

pub struct DatabaseFirst {
    pub db: Database,
    pub set: Statement,
    pub remove: Statement,
}

pub struct StatementsFirst {
    pub set: Statement,
    pub remove: Statement,
    pub db: Database,
}

pub fn store(db: Database) -> Result<(DatabaseFirst, StatementsFirst)> {
    Ok((
        DatabaseFirst {
            set: db.prepare("SELECT 1;")?,
            remove: db.prepare("SELECT 2;")?,
            db: db.clone(),
        },
        StatementsFirst {
            set: db.prepare("SELECT 1;")?,
            remove: db.prepare("SELECT 2;")?,
            db,
        },
    ))
}
