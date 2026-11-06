// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "parses and stringifies JSON through the V8 FFI")]

//! JSON parsing and stringifying, as C++ JSG's `jsg::Lock::parseJson()` and `serializeJson()`.
//!
//! These do what JavaScript's `JSON.parse()` and `JSON.stringify()` do without a reviver or a
//! replacer, but call V8 directly, so a script that replaces the `JSON` global doesn't affect
//! them.

use crate::Error;
use crate::Lock;
use crate::v8;

/// Parses `text` as JSON, as `JSON.parse(text)` does. Runs no JavaScript.
///
/// # Errors
///
/// If `text` is not valid JSON, the `SyntaxError` stays pending, and the error is one for which
/// [`Error::is_pending`] is true; so it is while an exception or a termination is pending. Return
/// it, as for [`v8::Local::<v8::Function>::call`]. If `text` is longer than
/// [`v8::String::MAX_LENGTH`] bytes, the error is [`Error::invalid_string_length`].
pub fn parse<'s>(lock: &mut Lock<'s>, text: &str) -> Result<v8::Local<'s, v8::Value>, Error> {
    let text = v8::String::new_from_str(lock, text).ok_or_else(Error::invalid_string_length)?;
    v8::call_into_js(lock, |lock| {
        // SAFETY: the lock guarantees the isolate is locked, a context is entered and a
        // HandleScope is active, and `text` belongs to its scope. The result belongs to the
        // lock's scope.
        Ok(unsafe {
            v8::Local::from_ffi_maybe(v8::ffi::value_json_parse(
                lock.isolate().as_ffi(),
                text.as_ffi(),
            ))
        })
    })
}

/// Stringifies `value` as JSON, as `JSON.stringify(value)` does.
///
/// A value that JSON can't represent, which `JSON.stringify()` returns `undefined` for (such as
/// `undefined`, a function or a symbol, or an object whose `toJSON()` returns one), gives the
/// string `"undefined"`, as `jsg::Lock::serializeJson()` does. Check the value first if that
/// matters.
///
/// # Errors
///
/// Stringifying runs JavaScript (getters, `toJSON()` and Proxy traps). If it throws, or `value`
/// has a cycle or a `BigInt` (a `TypeError`), or an exception or a termination is already
/// pending, the exception stays pending and the error is one for which [`Error::is_pending`] is
/// true. Return it, as for [`v8::Local::<v8::Function>::call`].
pub fn stringify<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> Result<String, Error> {
    stringify_with_gap(lock, value, "")
}

/// Stringifies `value` as JSON, indenting it by `gap`, as `JSON.stringify(value, null, gap)` does.
///
/// Each level is indented by `gap`'s first 10 UTF-16 code units, and an empty `gap` gives what
/// [`stringify`] does.
///
/// # Errors
///
/// As for [`stringify`]. If `gap` is longer than [`v8::String::MAX_LENGTH`] bytes, the error is
/// [`Error::invalid_string_length`].
pub fn stringify_with_gap<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
    gap: &str,
) -> Result<String, Error> {
    value.debug_assert_isolate(lock);
    let gap = v8::String::new_from_str(lock, gap).ok_or_else(Error::invalid_string_length)?;
    v8::call_into_js(lock, |lock| {
        let mut result = String::new();
        // SAFETY: the lock guarantees the isolate is locked, a context is entered and a
        // HandleScope is active, and `value` and `gap` belong to its scope.
        let ok = unsafe {
            v8::ffi::local_json_stringify(
                lock.isolate().as_ffi(),
                value.as_ffi(),
                gap.as_ffi(),
                &mut result,
            )
        }?;
        Ok(ok.then_some(result))
    })
}
