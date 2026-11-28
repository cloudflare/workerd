// `OpenMode` is a plain enum, so a `match` covers it without a wildcard arm.
use sqlite_rs::OpenMode;

pub const fn creates(mode: OpenMode) -> bool {
    match mode {
        OpenMode::ReadOnly | OpenMode::ReadWrite => false,
        OpenMode::Create | OpenMode::CreateWithParents => true,
    }
}
