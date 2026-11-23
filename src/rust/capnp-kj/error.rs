// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Conversions between capnp-rust errors and KJ exceptions.
//!
//! Both have the same four kinds. capnp-rust's decoding error kinds become `Failed`.
//!
//! A `capnp::Error` is only a kind and a description, but workerd's exceptions carry detail
//! entries that decide what callers do: whether a request reached a Durable Object, whether to
//! retry it, whether an error is the user's, why a script was killed (see `jsg/exception.h`,
//! `worker-interface.h` and `util/exception.h`). So that a Rust server that returns a C++
//! capability's error returns it whole, [`from_kj`] parks each exception that has details in a
//! per-thread table and marks the error's description with a token for it, which [`to_kj`] strips
//! and looks up to restore the details and the exception's file and line. The token is a few
//! characters from Unicode's private use area, which display as nothing, or as boxes; it survives
//! a server that adds context to the description, since [`to_kj`] finds it anywhere in it.
//!
//! The table keeps the latest [`MAX_PARKED`] exceptions of each thread, so an error that Rust holds
//! for longer, or sends to another thread, comes back without its details.

use std::cell::RefCell;
use std::collections::VecDeque;

use capnp::Error;
use capnp::ErrorKind;
use cxx::KjError;
use cxx::KjException;
use cxx::KjExceptionType;

/// How many exceptions each thread keeps for [`to_kj`] to restore.
const MAX_PARKED: usize = 64;

/// Starts a token; the token's id follows, in hex, one [`TOKEN_DIGITS`] character per digit.
const TOKEN_MARKER: char = '\u{E000}';
/// The first of 16 characters that stand for the hex digits of a token's id.
const TOKEN_DIGITS: u32 = 0xE010;

struct ParkedExceptions {
    next_id: u64,
    entries: VecDeque<(u64, KjError)>,
}

thread_local! {
    static PARKED: RefCell<ParkedExceptions> = const {
        RefCell::new(ParkedExceptions {
            next_id: 0,
            entries: VecDeque::new(),
        })
    };
}

fn park(exception: KjError) -> u64 {
    PARKED.with_borrow_mut(|parked| {
        let id = parked.next_id;
        parked.next_id += 1;
        if parked.entries.len() == MAX_PARKED {
            parked.entries.pop_front();
        }
        parked.entries.push_back((id, exception));
        id
    })
}

fn find_parked(id: u64) -> Option<KjError> {
    PARKED.with_borrow(|parked| {
        parked
            .entries
            .iter()
            .find(|(parked_id, _)| *parked_id == id)
            .map(|(_, exception)| exception.clone())
    })
}

fn push_token(description: &mut String, id: u64) {
    description.push(TOKEN_MARKER);
    let digits = if id == 0 {
        1
    } else {
        16 - (id.leading_zeros() / 4)
    };
    for shift in (0..digits).rev() {
        let digit = u32::try_from((id >> (shift * 4)) & 0xf).unwrap_or(0);
        if let Some(c) = char::from_u32(TOKEN_DIGITS + digit) {
            description.push(c);
        }
    }
}

fn token_digit(c: char) -> Option<u64> {
    let value = u32::from(c).checked_sub(TOKEN_DIGITS)?;
    (value < 16).then_some(u64::from(value))
}

/// Removes every token from `description`, and returns the id of the last.
fn strip_tokens(description: &str) -> (String, Option<u64>) {
    if !description.contains(TOKEN_MARKER) {
        return (description.to_owned(), None);
    }
    let mut stripped = String::with_capacity(description.len());
    let mut id = None;
    let mut chars = description.chars().peekable();
    while let Some(c) = chars.next() {
        if c != TOKEN_MARKER {
            stripped.push(c);
            continue;
        }
        let mut value: u64 = 0;
        while let Some(digit) = chars.peek().copied().and_then(token_digit) {
            value = (value << 4) | digit;
            chars.next();
        }
        id = Some(value);
    }
    (stripped, id)
}

/// Converts the exception of a failed C++ call.
pub fn from_kj(e: &KjException) -> Error {
    let kind = match e.r#type() {
        KjExceptionType::Failed => ErrorKind::Failed,
        KjExceptionType::Overloaded => ErrorKind::Overloaded,
        KjExceptionType::Disconnected => ErrorKind::Disconnected,
        KjExceptionType::Unimplemented => ErrorKind::Unimplemented,
    };
    let mut extra = e.what().to_owned();
    if let Some(details) = e.details() {
        let mut original = KjError::new(e.r#type(), String::new()).with_details(details);
        if let Ok(file) = e.file().to_str() {
            original =
                original.with_location(file.to_owned(), u32::try_from(e.line()).unwrap_or(0));
        }
        push_token(&mut extra, park(original));
    }
    Error { kind, extra }
}

/// Converts an error for C++. An error that came from C++ gets back the details and location of
/// its exception; its kind and description are the error's, which Rust may have changed.
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
    let (description, id) = strip_tokens(&description);
    let mut error = KjError::new(exception_type, description);
    if let Some(original) = id.and_then(find_parked) {
        if let Some(details) = original.details() {
            error = error.with_details(details.clone());
        }
        if let (Some(file), Some(line)) = (original.file(), original.line()) {
            error = error.with_location(file.to_owned(), line);
        }
    }
    error
}

#[cfg(test)]
#[path = "error-test.rs"]
mod tests;
