// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/// An optional `T` that a value of the wrong type leaves absent, like C++ JSG's
/// `jsg::LenientOptional<T>`.
///
/// `undefined` is absent. Any other value, `null` included, goes through `T`'s conversion,
/// and a top-level type mismatch is absent too rather than an error. So `null` is `"null"`
/// for a `LenientOption<String>`, but absent for a `LenientOption<jsg::Rc<R>>`. Errors from
/// nested conversions or from JavaScript run during the conversion still propagate, and so
/// do errors from a coercing `T`: a `LenientOption<u32>` takes `null` as `0`, and `'abc'` or
/// `-1` is a `TypeError`, as for C++ `LenientOptional<uint32_t>`.
///
/// An absent value converts to `undefined`. As a method argument it may be omitted, and as a
/// `#[jsg_struct]` field it is optional, as an [`Option<T>`] is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct LenientOption<T>(pub Option<T>);

impl<T> LenientOption<T> {
    /// Returns the contained `Option<T>`.
    pub fn into_inner(self) -> Option<T> {
        self.0
    }
}

/// Derefs to the contained `Option<T>`, as C++ `LenientOptional<T>` is a `kj::Maybe<T>`.
impl<T> std::ops::Deref for LenientOption<T> {
    type Target = Option<T>;

    fn deref(&self) -> &Option<T> {
        &self.0
    }
}

impl<T> std::ops::DerefMut for LenientOption<T> {
    fn deref_mut(&mut self) -> &mut Option<T> {
        &mut self.0
    }
}

impl<T> Default for LenientOption<T> {
    fn default() -> Self {
        Self(None)
    }
}

impl<T> From<Option<T>> for LenientOption<T> {
    fn from(value: Option<T>) -> Self {
        Self(value)
    }
}

impl<T> From<LenientOption<T>> for Option<T> {
    fn from(value: LenientOption<T>) -> Self {
        value.0
    }
}
