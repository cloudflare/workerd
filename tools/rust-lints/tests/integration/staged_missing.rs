#![deny(unsafe_code)]

include!("generated.rs");
include!("included_missing.rs");

#[path = "external_missing.rs"]
mod external;

mod inline {}
