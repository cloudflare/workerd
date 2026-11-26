// SQLite runs a prepared statement once at a time, so its rows borrow it mutably.
use sqlite_rs::Statement;

pub fn twice(statement: &mut Statement) {
    let first = statement.run(&[]);
    let second = statement.run(&[]); //~ ERROR E0499
    drop((first, second));
}
