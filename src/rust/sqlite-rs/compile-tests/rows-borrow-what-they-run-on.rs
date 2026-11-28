// Rows read the statement or database handle they run on, so they cannot outlive it.
use sqlite_rs::Database;
use sqlite_rs::Result;
use sqlite_rs::Rows;
use sqlite_rs::Statement;

pub fn statement_rows(db: &Database) -> Result<Rows<'_>> {
    let mut statement = db.prepare("SELECT 1;")?;
    statement.run(&[]) //~ ERROR E0515
}

pub fn dropped_statement(mut statement: Statement) {
    let rows = statement.run(&[]);
    drop(statement); //~ ERROR E0505
    drop(rows);
}

pub fn database_rows(db: Database) -> Result<Rows<'static>> {
    db.run("SELECT 1;", &[]) //~ ERROR E0515
}

pub fn dropped_database(db: Database) {
    let rows = db.run("SELECT 1;", &[]);
    drop(db); //~ ERROR E0505
    drop(rows);
}
