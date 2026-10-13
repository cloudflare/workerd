//! The FFI between the Rust entry point and the C++ side: the config compiler (config-compiler.h)
//! and what the driver (bootstrap.h) knows about the build. The `serve` and `test` commands go
//! through the server crate's bridge instead (`workerd_server::entry`).
//!
//! C++ calls back into [`Process`] to register the files a config depends on for `--watch`.

#![allow(
    unsafe_code,
    reason = "holds a cxx bridge, which expands to unsafe FFI glue"
)]

pub use crate::process::Process;

#[cxx::bridge(namespace = "workerd::server::cli")]
pub mod ffi {
    /// A parse error in a schema file. `line` and `column` are 1-based; `end_column` is 0 when
    /// the error has no extent on the line.
    struct ConfigParseError {
        file: String,
        line: u32,
        column: u32,
        end_column: u32,
        message: String,
    }

    struct CompiledConfig {
        /// The config as an encoded message (segment table, then segments), in 8-byte words.
        /// Empty if there were parse errors.
        message: Vec<u64>,
        errors: Vec<ConfigParseError>,
    }

    unsafe extern "C++" {
        include!("workerd/server/factory/bootstrap.h");
        include!("workerd/server/config-compiler.h");

        /// Compiles a config written as a Cap'n Proto schema file. Every file it is about to read
        /// is first registered with `process.watch_file()`, so `--watch` cannot miss a change
        /// made while compiling. An error is a problem with the file or the arguments themselves
        /// (not found, no such constant, ...); parse errors in the file's contents are returned
        /// in `errors`.
        fn compile_config(
            path: &str,
            import_paths: &[String],
            const_name: KjMaybe<&str>,
            process: &Process,
        ) -> Result<CompiledConfig>;

        /// The release date this binary was built from, as ASCII.
        fn release_version() -> &'static [u8];
        fn perfetto_supported() -> bool;
        fn fuzzilli_supported() -> bool;
        /// The package lock file of the current Pyodide release.
        fn pyodide_lock() -> Result<String>;
    }

    extern "Rust" {
        type Process;

        /// Whether `--watch` was given.
        fn is_watching(self: &Process) -> bool;

        /// Adds a file the config depends on to the watched set (native path bytes); a no-op
        /// without `--watch`.
        fn watch_file(self: &Process, path: &[u8]) -> Result<()>;

    }
}
