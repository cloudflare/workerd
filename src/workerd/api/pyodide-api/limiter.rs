// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// `pyodide-internal:limiter` when the embedder limits nothing: the disabled C++
/// `SimplePythonLimiter`.
///
/// `src/pyodide/python-entrypoint-helper.ts` tells it when a Python Worker's startup begins and
/// finishes, and it does nothing with either, but converts its arguments as the C++ class does.
#[jsg_resource]
#[derive(Default)]
pub struct SimplePythonLimiter;

impl SimplePythonLimiter {
    /// A limiter that limits nothing.
    #[must_use]
    pub fn disabled(lock: &mut jsg::Lock<'_>) -> jsg::Rc<Self> {
        jsg::Rc::new(lock, Self)
    }
}

#[jsg_resource]
impl SimplePythonLimiter {
    /// Called when the startup begins.
    #[jsg_method]
    pub fn begin_startup(&self) {}

    /// Called when the startup finishes. `snapshotType` names the memory snapshot that the
    /// startup restored. It is required, but may be `null` or `undefined`, and any other value is
    /// converted to a string, as for the C++ `kj::Maybe<kj::String>`.
    #[jsg_method]
    pub fn finish_startup(&self, _snapshot_type: jsg::Nullable<String>) {}
}

#[cfg(test)]
#[path = "limiter-test.rs"]
mod tests;
