// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::MemoryInfo;
use crate::MemoryTracker;
use crate::Traced;
use crate::rtti;

/// The remaining arguments of a call, each converted to `T`: a rest parameter, like C++ JSG's
/// `jsg::Arguments<T>`.
///
/// As the last parameter of a `#[jsg_method]` or a `#[jsg_constructor]`, it takes every argument
/// from its position on, converting each one as a parameter of type `T` would be converted, and
/// failing with the same `TypeError`, which names the rest parameter's position as C++ JSG's does.
/// Without any, it is empty. It does not count towards the function's `.length`, and TypeScript
/// declares it as `...name: any[]`, as for C++ JSG. It converts only from the arguments of a call,
/// so it has no `FromJS`; a JavaScript array is a `Vec<T>`.
///
/// # Examples
///
/// ```ignore
/// #[jsg_method]
/// pub fn exec(&self, query: String, bindings: jsg::Arguments<Binding>) {
///     for binding in bindings {
///         // ...
///     }
/// }
/// ```
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Arguments<T>(Vec<T>);

impl<T> Arguments<T> {
    /// Returns the arguments, in order.
    #[must_use]
    pub fn into_inner(self) -> Vec<T> {
        self.0
    }
}

impl<T> Default for Arguments<T> {
    fn default() -> Self {
        Self(Vec::new())
    }
}

impl<T> From<Vec<T>> for Arguments<T> {
    fn from(arguments: Vec<T>) -> Self {
        Self(arguments)
    }
}

impl<T> std::ops::Deref for Arguments<T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        &self.0
    }
}

impl<T> IntoIterator for Arguments<T> {
    type Item = T;
    type IntoIter = std::vec::IntoIter<T>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.into_iter()
    }
}

impl<'a, T> IntoIterator for &'a Arguments<T> {
    type Item = &'a T;
    type IntoIter = std::slice::Iter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.iter()
    }
}

/// Reported as the `Vec` of the arguments.
impl<T: MemoryInfo> MemoryInfo for Arguments<T> {
    fn track_as_field(
        &self,
        tracker: &mut MemoryTracker<'_>,
        edge_name: Option<&'static std::ffi::CStr>,
    ) {
        self.0.track_as_field(tracker, edge_name);
    }
}

impl<T: Traced> Traced for Arguments<T> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        self.0.trace(visitor);
    }
}

impl<T: crate::HoldsNoGcReferences> crate::HoldsNoGcReferences for Arguments<T> {}

/// Described as C++ RTTI describes `jsg::Arguments<T>`, without its element type.
impl<T> rtti::TypeInfo for Arguments<T> {
    fn describe(_cx: &mut rtti::Context) -> rtti::Type {
        rtti::Type::Varargs
    }
}
