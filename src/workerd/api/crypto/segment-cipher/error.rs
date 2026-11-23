// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Errors as JavaScript sees them. C++ JSG turns a `kj::Exception` described as
//! `jsg.<Type>: <message>` or `jsg.DOMException(<name>): <message>` into that error.

pub type Result<T> = std::result::Result<T, cxx::KjError>;

fn error(description: String) -> cxx::KjError {
    cxx::KjError::new(cxx::KjExceptionType::Failed, description)
}

pub fn type_error(message: &str) -> cxx::KjError {
    error(format!("jsg.TypeError: {message}"))
}

pub fn range_error(message: &str) -> cxx::KjError {
    error(format!("jsg.RangeError: {message}"))
}

pub fn operation_error(message: &str) -> cxx::KjError {
    error(format!("jsg.DOMException(OperationError): {message}"))
}

pub fn not_supported_error(message: &str) -> cxx::KjError {
    error(format!("jsg.DOMException(NotSupportedError): {message}"))
}
