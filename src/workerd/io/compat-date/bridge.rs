//! The bridge that `workerd/io/compatibility-date.c++` wraps.
//!
//! Strings cross as bytes, since a `kj::String` need not be UTF-8, and diagnostics quote them
//! byte for byte.

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue"
)]

use capnp::message::ReaderOptions;
use capnp::message::SegmentArray;
use compatibility_date_capnp::compatibility_flags;

use crate::compile;
use crate::date::CompatDate;

#[cxx::bridge(namespace = "workerd::rust::compat_date")]
mod ffi {
    enum DateValidation {
        CodeVersion,
        CurrentDateForCloudflare,
        FutureForTest,
    }

    enum Decompile {
        All,
        NeededByFl,
    }

    /// One string of a list, borrowed from C++.
    struct Bytes<'a> {
        bytes: &'a [u8],
    }

    struct CompileOptions {
        date_validation: DateValidation,
        allow_experimental: bool,
        main_module_is_python: bool,
    }

    enum DiagnosticKind {
        Error,
        Warning,
    }

    struct Diagnostic {
        kind: DiagnosticKind,
        /// Not necessarily UTF-8.
        message: Vec<u8>,
    }

    struct CompileResult {
        /// Indexed by field index (code order).
        enabled: Vec<bool>,
        /// In the order the compiler found them.
        diagnostics: Vec<Diagnostic>,
    }

    extern "Rust" {
        /// Compiles a date and flags. Fails only if the schema could not be read.
        fn compile_compatibility_flags(
            compat_date: &[u8],
            flags: &[Bytes<'_>],
            allowed_experimental_flags: &[Bytes<'_>],
            options: &CompileOptions,
        ) -> Result<CompileResult>;

        /// Returns the field indices of the set flags that have an enable flag, in code order.
        /// `canonical` is `capnp::canonicalize()` of a `CompatibilityFlags`.
        fn decompile_compatibility_flags(canonical: &[u8], which: Decompile) -> Result<Vec<u16>>;

        /// Returns the date in canonical form, or an empty string if it is not a valid date.
        fn normalize_compat_date(date: &[u8]) -> String;

        fn current_date_str() -> String;

        fn newest_compat_date_str() -> String;
    }
}

#[derive(Debug, thiserror::Error)]
enum Error {
    #[error(transparent)]
    Schema(#[from] crate::table::SchemaError),
    #[error("reading CompatibilityFlags: {0}")]
    Capnp(#[from] capnp::Error),
    #[error("unknown enum value {0}")]
    UnknownEnum(u8),
}

fn compile_compatibility_flags(
    compat_date: &[u8],
    flags: &[ffi::Bytes<'_>],
    allowed_experimental_flags: &[ffi::Bytes<'_>],
    options: &ffi::CompileOptions,
) -> Result<ffi::CompileResult, Error> {
    let allowed: Vec<&[u8]> = allowed_experimental_flags.iter().map(|f| f.bytes).collect();
    let date_validation = match options.date_validation {
        ffi::DateValidation::CodeVersion => compile::DateValidation::CodeVersion,
        ffi::DateValidation::CurrentDateForCloudflare => {
            compile::DateValidation::CurrentDateForCloudflare
        }
        ffi::DateValidation::FutureForTest => compile::DateValidation::FutureForTest,
        ffi::DateValidation { repr } => return Err(Error::UnknownEnum(repr)),
    };
    let compiled = compile::compile(
        compat_date,
        flags.iter().map(|f| f.bytes),
        &compile::CompileOptions {
            date_validation,
            allow_experimental: options.allow_experimental,
            allowed_experimental_flags: &allowed,
            main_module_is_python: options.main_module_is_python,
        },
    )?;
    Ok(ffi::CompileResult {
        enabled: compiled.enabled,
        diagnostics: compiled
            .diagnostics
            .into_iter()
            .map(|d| match d {
                compile::Diagnostic::Error(message) => ffi::Diagnostic {
                    kind: ffi::DiagnosticKind::Error,
                    message,
                },
                compile::Diagnostic::Warning(message) => ffi::Diagnostic {
                    kind: ffi::DiagnosticKind::Warning,
                    message,
                },
            })
            .collect(),
    })
}

fn decompile_compatibility_flags(
    canonical: &[u8],
    which: ffi::Decompile,
) -> Result<Vec<u16>, Error> {
    let which = match which {
        ffi::Decompile::All => compile::Decompile::All,
        ffi::Decompile::NeededByFl => compile::Decompile::NeededByFl,
        ffi::Decompile { repr } => return Err(Error::UnknownEnum(repr)),
    };
    let segments = [canonical];
    // The bytes are C++'s own canonical encoding of a small struct, not untrusted input.
    let message = capnp::message::Reader::new(SegmentArray::new(&segments), ReaderOptions::new());
    let input = message.get_root::<compatibility_flags::Reader<'_>>()?;
    Ok(compile::decompile_fields(input, which)?)
}

fn normalize_compat_date(date: &[u8]) -> String {
    std::str::from_utf8(date)
        .ok()
        .and_then(CompatDate::parse)
        .map(|d| d.to_string())
        .unwrap_or_default()
}

fn current_date_str() -> String {
    CompatDate::today().to_string()
}

fn newest_compat_date_str() -> String {
    compile::newest_compat_date().to_string()
}
