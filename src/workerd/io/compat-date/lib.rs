//! The Rust compiler of compatibility dates and flags.
//!
//! Turns a compatibility date and a list of flags into `CompatibilityFlags`, reading what each
//! flag means from the annotations in `compatibility-date.capnp`, and turns `CompatibilityFlags`
//! back into flag names. C++ reaches it through `workerd/io/compatibility-date.h`, which wraps
//! the bridge in `bridge.rs`.

mod bridge;
pub mod compile;
pub mod date;
pub mod table;

pub use compile::CompileOptions;
pub use compile::Compiled;
pub use compile::DateValidation;
pub use compile::Decompile;
pub use compile::Diagnostic;
pub use compile::compile;
pub use compile::decompile;
pub use compile::maximum_compat_date;
pub use compile::newest_compat_date;
pub use date::CompatDate;
pub use table::SchemaError;
