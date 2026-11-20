// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Conversions between capnp-rust errors and KJ exceptions.
//!
//! Both have the same four kinds. capnp-rust's decoding error kinds become `Failed`.

use capnp::Error;
use capnp::ErrorKind;
use cxx::KjError;
use cxx::KjException;
use cxx::KjExceptionType;

/// Converts the exception of a failed C++ call.
pub fn from_kj(e: &KjException) -> Error {
    let kind = match e.r#type() {
        KjExceptionType::Failed => ErrorKind::Failed,
        KjExceptionType::Overloaded => ErrorKind::Overloaded,
        KjExceptionType::Disconnected => ErrorKind::Disconnected,
        KjExceptionType::Unimplemented => ErrorKind::Unimplemented,
    };
    Error {
        kind,
        extra: e.what().to_owned(),
    }
}

pub fn to_kj(e: Error) -> KjError {
    let exception_type = match e.kind {
        ErrorKind::Overloaded => KjExceptionType::Overloaded,
        ErrorKind::Disconnected => KjExceptionType::Disconnected,
        ErrorKind::Unimplemented => KjExceptionType::Unimplemented,
        _ => KjExceptionType::Failed,
    };
    let description = match e.kind {
        // The kind already says what the error is; keep the description as C++ wrote it.
        ErrorKind::Failed
        | ErrorKind::Overloaded
        | ErrorKind::Disconnected
        | ErrorKind::Unimplemented => e.extra,
        _ => e.to_string(),
    };
    KjError::new(exception_type, description)
}
