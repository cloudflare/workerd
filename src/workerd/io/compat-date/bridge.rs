//! The bridge that `workerd/io/compatibility-date.c++` wraps.
//!
//! Strings cross as bytes, since a `kj::String` need not be UTF-8, and diagnostics quote them
//! byte for byte, as the C++ compiler did: a list crosses as its strings' bytes, end to end, and
//! their lengths.

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue"
)]

use capnp::message::ReaderOptions;
use capnp::message::SegmentArray;
use compatibility_date_capnp::compatibility_flags;

use crate::compile;
use crate::date::CompatDate;
use crate::table::FlagTable;

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
        /// Compiles a date and flags; `flags` and `allowed_experimental_flags` are lists as the
        /// module comment describes. Fails only if the schema could not be read.
        fn compile_compatibility_flags(
            compat_date: &[u8],
            flag_bytes: &[u8],
            flag_lengths: &[usize],
            allowed_experimental_bytes: &[u8],
            allowed_experimental_lengths: &[usize],
            options: &CompileOptions,
        ) -> Result<CompileResult>;

        /// Returns the field indices of the set flags that have an enable flag, in code order.
        /// `canonical` is `capnp::canonicalize()` of a `CompatibilityFlags`.
        fn decompile_compatibility_flags(canonical: &[u8], which: Decompile) -> Result<Vec<u16>>;

        /// The `$compatEnableFlag` of a field that has one. It lives in the schema, which is
        /// static, and is followed by a NUL, as Cap'n Proto text always is.
        fn enable_flag_name(field_index: u16) -> Result<&'static str>;

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
    #[error("a list of {lengths} lengths that sum to more than its {bytes} bytes")]
    List { bytes: usize, lengths: usize },
    #[error("field {0} has no enable flag")]
    NoEnableFlag(u16),
    #[error("unknown enum value {0}")]
    UnknownEnum(u8),
}

fn split<'a>(bytes: &'a [u8], lengths: &[usize]) -> Result<Vec<&'a [u8]>, Error> {
    let error = || Error::List {
        bytes: bytes.len(),
        lengths: lengths.len(),
    };
    let mut rest = bytes;
    let mut strings = Vec::with_capacity(lengths.len());
    for &len in lengths {
        let (head, tail) = rest.split_at_checked(len).ok_or_else(error)?;
        strings.push(head);
        rest = tail;
    }
    if !rest.is_empty() {
        return Err(error());
    }
    Ok(strings)
}

fn compile_compatibility_flags(
    compat_date: &[u8],
    flag_bytes: &[u8],
    flag_lengths: &[usize],
    allowed_experimental_bytes: &[u8],
    allowed_experimental_lengths: &[usize],
    options: &ffi::CompileOptions,
) -> Result<ffi::CompileResult, Error> {
    let flags = split(flag_bytes, flag_lengths)?;
    let allowed = split(allowed_experimental_bytes, allowed_experimental_lengths)?;
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
        flags,
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

fn enable_flag_name(field_index: u16) -> Result<&'static str, Error> {
    FlagTable::get()?
        .fields
        .get(usize::from(field_index))
        .and_then(|info| info.enable_flag)
        .ok_or(Error::NoEnableFlag(field_index))
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
