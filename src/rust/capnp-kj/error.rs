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
//! A token names its entry by a random 64-bit id, as descriptions can hold text from outside the
//! process, such as a JavaScript error's message: text that carries a token of its own can't name
//! another exception's entry without guessing its id. The table keeps the latest [`MAX_PARKED`]
//! exceptions of each thread, so an error that Rust holds for longer, or sends to another thread,
//! comes back without its details.

use std::cell::RefCell;
use std::collections::VecDeque;
use std::hash::BuildHasher;
use std::hash::RandomState;

use capnp::Error;
use capnp::ErrorKind;
use cxx::KjError;
use cxx::KjException;
use cxx::KjExceptionType;

/// How many exceptions each thread keeps for [`to_kj`] to restore.
const MAX_PARKED: usize = 64;

/// Starts a token, which [`TOKEN_LENGTH`] digits follow: its id in hex, one [`TOKEN_DIGITS`]
/// character per digit.
const TOKEN_MARKER: char = '\u{E000}';
/// The first of 16 characters that stand for the hex digits of a token's id.
const TOKEN_DIGITS: u32 = 0xE010;
/// How many digits follow a token's marker.
const TOKEN_LENGTH: usize = 16;

struct ParkedExceptions {
    /// Draws the entries' ids: a hash with this thread's random keys of a count of entries.
    ids: RandomState,
    next: u64,
    entries: VecDeque<(u64, KjError)>,
}

thread_local! {
    static PARKED: RefCell<ParkedExceptions> = RefCell::new(ParkedExceptions {
        ids: RandomState::new(),
        next: 0,
        entries: VecDeque::new(),
    });
}

/// Parks `exception` and returns its id, or none if the thread is exiting and has no table.
fn park(exception: KjError) -> Option<u64> {
    PARKED
        .try_with(|parked| {
            let mut parked = parked.borrow_mut();
            let id = parked.ids.hash_one(parked.next);
            parked.next += 1;
            if parked.entries.len() == MAX_PARKED {
                parked.entries.pop_front();
            }
            parked.entries.push_back((id, exception));
            id
        })
        .ok()
}

fn find_parked(id: u64) -> Option<KjError> {
    PARKED
        .try_with(|parked| {
            parked
                .borrow()
                .entries
                .iter()
                .find(|(parked_id, _)| *parked_id == id)
                .map(|(_, exception)| exception.clone())
        })
        .ok()
        .flatten()
}

fn push_token(description: &mut String, id: u64) {
    description.push(TOKEN_MARKER);
    for shift in (0..TOKEN_LENGTH).rev() {
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

/// The id of the token at the start of `rest`, which follows a marker.
fn token_id(rest: &str) -> Option<u64> {
    let mut id = 0;
    let mut digits = rest.chars();
    for _ in 0..TOKEN_LENGTH {
        id = (id << 4) | token_digit(digits.next()?)?;
    }
    Some(id)
}

/// Removes every token from `description`, and returns the ids of those it removed, in order.
/// Text that only resembles a token stays.
fn strip_tokens(description: &str) -> (String, Vec<u64>) {
    if !description.contains(TOKEN_MARKER) {
        return (description.to_owned(), Vec::new());
    }
    let mut stripped = String::with_capacity(description.len());
    let mut ids = Vec::new();
    let mut rest = description;
    while let Some(start) = rest.find(TOKEN_MARKER) {
        let after = &rest[start + TOKEN_MARKER.len_utf8()..];
        stripped.push_str(&rest[..start]);
        if let Some(id) = token_id(after) {
            ids.push(id);
            // Each digit is a three-byte character.
            rest = &after[TOKEN_LENGTH * 3..];
        } else {
            stripped.push(TOKEN_MARKER);
            rest = after;
        }
    }
    stripped.push_str(rest);
    (stripped, ids)
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
        if let Some(id) = park(original) {
            push_token(&mut extra, id);
        }
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
    let (description, ids) = strip_tokens(&description);
    let mut error = KjError::new(exception_type, description);
    // The latest exception that Rust built on is the one the error came from.
    if let Some(original) = ids.into_iter().rev().find_map(find_parked) {
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
