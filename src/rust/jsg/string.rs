// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::ops::Deref;

use crate::Error;
use crate::FromJS;
use crate::Lock;
use crate::ToJS;
use crate::Type;
use crate::UnionMember;
use crate::UnionTypes;
use crate::v8;

/// A JavaScript string that stays on the V8 heap, like C++ JSG's `JsString`.
///
/// A method parameter converts any value with JavaScript's `ToString`, running JavaScript
/// where needed and rejecting symbols. Unlike a Rust [`String`], this does not copy or decode
/// the string, so lone UTF-16 surrogates are preserved. Returning it passes the string back
/// unchanged. A [`v8::Local<'s, v8::String>`] parameter, in contrast, accepts only strings.
///
/// Dereferences to the string handle for operations such as `length()`, `write()` and
/// `to_string(lock)`. Convert a string handle with [`JsString::from`]. Like the handle, it
/// cannot outlive its scope; use a [`v8::Global`] or [`v8::TracedGlobal`] to keep it longer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct JsString<'s>(v8::Local<'s, v8::String>);

impl<'s> Deref for JsString<'s> {
    type Target = v8::Local<'s, v8::String>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl<'s> From<v8::Local<'s, v8::String>> for JsString<'s> {
    fn from(value: v8::Local<'s, v8::String>) -> Self {
        Self(value)
    }
}

impl<'s> From<JsString<'s>> for v8::Local<'s, v8::String> {
    fn from(value: JsString<'s>) -> Self {
        value.0
    }
}

impl<'s> From<JsString<'s>> for v8::Local<'s, v8::Value> {
    fn from(value: JsString<'s>) -> Self {
        value.0.into()
    }
}

impl Type for JsString<'_> {
    const UNION_TYPES: UnionTypes = UnionTypes::STRING;

    fn class_name() -> &'static str {
        "string"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_string()
    }
}

impl<'s> FromJS<'s> for JsString<'s> {
    type ResultType = Self;

    const UNION_MEMBER: UnionMember = UnionMember::String;

    fn type_name() -> String {
        "string".to_owned()
    }

    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<Self, Error> {
        value.to_js_string(lock).map(Self)
    }
}

impl<'s> ToJS<'s> for JsString<'s> {
    fn to_js(self, _lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        Ok(self.into())
    }
}
