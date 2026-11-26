// A connection, its statements and their rows stay on the thread that opened them.
use sqlite_rs::Database;
use sqlite_rs::Rows;
use sqlite_rs::Statement;

fn send<T: Send>(_: T) {}

pub fn database(db: Database) {
    send(db); //~ ERROR E0277
}

// One error for each part that is not `Send`: the C++ statement and the database handle.
pub fn statement(statement: Statement) {
    send(statement); //~ ERROR E0277

    //~| ERROR E0277
}

pub fn rows(rows: Rows<'_>) {
    send(rows); //~ ERROR E0277
}
