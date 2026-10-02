#![forbid(unsafe_code)]

mod inherited {}

#[path = "modules/outer.rs"]
mod external_inherited;
