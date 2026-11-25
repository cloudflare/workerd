// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and its functions take C++'s raw isolate"
)]

//! The bridge to `ffi.h`, and the functions `api/per-isolate-bootstrap.c++` calls.

use jsg::v8;

use crate::Scope;
use crate::State;
use crate::normalize;

// Every function that takes an isolate takes the isolate of the caller's lock, locked by this
// thread with a context entered and a `HandleScope` open, and handles in that scope.
#[cxx::bridge(namespace = "workerd::api::per_isolate_bootstrap")]
#[expect(
    clippy::missing_safety_doc,
    reason = "cxx bridge extern decls; the comment above states their uniform contract"
)]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    unsafe extern "C++" {
        include!("workerd/api/per-isolate-bootstrap/ffi.h");

        /// The scripts the bootstrap of a context runs (`ffi.h`).
        type ScriptHost;

        /// A script of a bootstrap bundle, C++ `jsg::Module::Reader`.
        type Script;

        fn in_embedder_namespace(self: &ScriptHost, specifier: &str) -> bool;
        fn find_workerd_script(self: &ScriptHost, specifier: &str) -> *const Script;
        fn find_embedder_script(self: &ScriptHost, specifier: &str) -> *const Script;
        fn embedder_entry_point(self: &ScriptHost) -> &str;
        unsafe fn module_source(
            self: &ScriptHost,
            isolate: *mut Isolate,
            script: &Script,
        ) -> MaybeLocal;
        unsafe fn api_natives(self: &ScriptHost, isolate: *mut Isolate) -> MaybeLocal;

        fn bootstrap_function_params() -> Vec<String>;
        fn script_source(script: &Script) -> &'static [u8];
        fn script_compile_cache<'a>(script: &'a Script) -> &'a [u8];

        unsafe fn install_state(isolate: *mut Isolate, state: Box<State>) -> Result<*const State>;
        unsafe fn current_state(isolate: *mut Isolate) -> *const State;
        unsafe fn snapshot_state_index(isolate: *mut Isolate, index: &mut usize) -> bool;
        unsafe fn set_snapshot_state_index(isolate: *mut Isolate, index: usize);
    }

    extern "Rust" {
        /// The state of one context's bootstrap (`lib.rs`).
        type State;

        unsafe fn run(
            isolate: *mut Isolate,
            host: UniquePtr<ScriptHost>,
            compat_flags: &[u8],
        ) -> MaybeLocal;
        unsafe fn stash_for_snapshot(isolate: *mut Isolate) -> MaybeLocal;
        // The lookups take the state C++ found in the current context, and the specifier and
        // name as bytes, which cost C++ no UTF-8 check through the bridge.
        unsafe fn loaded_exports(
            isolate: *mut Isolate,
            state: &State,
            specifier: &[u8],
        ) -> MaybeLocal;
        unsafe fn require_exports(
            isolate: *mut Isolate,
            state: &State,
            specifier: &[u8],
        ) -> MaybeLocal;
        unsafe fn exported_function(
            isolate: *mut Isolate,
            state: &State,
            specifier: &[u8],
            name: &[u8],
        ) -> MaybeLocal;
        unsafe fn set_exports_for_test(
            isolate: *mut Isolate,
            state: &State,
            specifier: &[u8],
            exports: Local,
        );
    }
}

/// Runs `f` with a lock for the caller's `HandleScope`, and returns its handle to C++. A panic
/// throws an internal error and terminates execution, and the result is then empty.
///
/// # Safety
/// The isolate is locked by this thread with a context entered and a `HandleScope` open.
unsafe fn with_lock<F>(isolate: *mut bridge::Isolate, f: F) -> bridge::MaybeLocal
where
    F: for<'s> FnOnce(&mut jsg::Lock<'s>) -> Option<v8::Local<'s, v8::Value>>,
{
    // SAFETY: forwarded from the caller.
    let scope = std::pin::pin!(unsafe { jsg::CallbackScope::new(isolate) });
    jsg::catch_panic(scope, |lock| {
        let local = f(lock);
        // SAFETY: the handle lives in the caller's `HandleScope`.
        unsafe { v8::Local::into_ffi_maybe(local) }
    })
    .unwrap_or_default()
}

/// Bootstraps the current context; see `crate::run`. Returns `undefined`, or an empty handle
/// with an exception pending.
///
/// # Safety
/// See the bridge.
unsafe fn run(
    isolate: *mut bridge::Isolate,
    host: cxx::UniquePtr<bridge::ScriptHost>,
    compat_flags: &[u8],
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            crate::run(lock, host, compat_flags)?;
            Ok::<_, jsg::Error>(lock.undefined())
        })
    }
}

/// Zygote only: pins the bootstrap's JavaScript state in the current context into the
/// snapshot, and records where in the isolate's snapshot artifact. Returns `undefined`, or an
/// empty handle with an exception pending.
///
/// # Safety
/// See the bridge.
unsafe fn stash_for_snapshot(isolate: *mut bridge::Isolate) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            crate::stash_for_snapshot(lock)?;
            Ok::<_, jsg::Error>(lock.undefined())
        })
    }
}

/// `bytes` as a specifier or name: UTF-8, as every script's name is. A name that isn't names
/// nothing.
fn utf8(bytes: &[u8]) -> Option<&str> {
    std::str::from_utf8(bytes).ok()
}

/// The exports of the loaded module `specifier` of `state`, or an empty handle. Runs no
/// JavaScript and throws nothing.
///
/// # Safety
/// See the bridge.
unsafe fn loaded_exports(
    isolate: *mut bridge::Isolate,
    state: &State,
    specifier: &[u8],
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        with_lock(isolate, |lock| {
            state.loaded(lock, normalize(utf8(specifier)?))
        })
    }
}

/// The exports of workerd's module `specifier` of `state`, loading it first if nothing has
/// required it. Or an empty handle with an exception pending.
///
/// # Safety
/// See the bridge.
unsafe fn require_exports(
    isolate: *mut bridge::Isolate,
    state: &State,
    specifier: &[u8],
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            let specifier = utf8(specifier).ok_or_else(|| {
                crate::internal_error(format!(
                    "per-isolate bootstrap: specifier is not UTF-8: {}",
                    String::from_utf8_lossy(specifier)
                ))
            })?;
            let specifier = normalize(specifier);
            match state.loaded(lock, specifier) {
                Some(exports) => Ok(exports),
                None => state.require(lock, specifier, Scope::Workerd),
            }
        })
    }
}

/// The function property `name` of the exports of the loaded module `specifier` of `state`, or
/// `undefined` if there is none. If reading the property throws, the handle is empty with the
/// exception pending.
///
/// # Safety
/// See the bridge.
unsafe fn exported_function(
    isolate: *mut bridge::Isolate,
    state: &State,
    specifier: &[u8],
    name: &[u8],
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            let (Some(specifier), Some(name)) = (utf8(specifier), utf8(name)) else {
                return Ok::<_, jsg::Error>(lock.undefined());
            };
            Ok(match state.function(lock, normalize(specifier), name)? {
                Some(function) => function.into(),
                None => lock.undefined(),
            })
        })
    }
}

/// Makes `exports` the exports of the module `specifier` of `state`.
///
/// # Safety
/// See the bridge.
unsafe fn set_exports_for_test(
    isolate: *mut bridge::Isolate,
    state: &State,
    specifier: &[u8],
    exports: bridge::Local,
) {
    // SAFETY: forwarded from the caller.
    unsafe {
        with_lock(isolate, |lock| {
            // SAFETY: `exports` is a handle in the caller's scope.
            let exports = v8::Local::<v8::Value>::from_ffi(exports);
            state.set_exports(lock, normalize(utf8(specifier)?), exports);
            None
        });
    }
}

// Safe calls over the bridge, for the loader (`lib.rs`), which holds no `unsafe` of its own.

/// The bootstrap state of the current context, or `None`.
///
/// The state lives until C++ frees it, when the context is disposed or, in a zygote, once its
/// state is stashed, neither of which can happen while Rust code entered in the context runs. So
/// the reference is valid for the caller's call.
pub fn current_state<'a>(lock: &mut jsg::Lock<'_>) -> Option<&'a State> {
    // SAFETY: the lock's isolate is locked with a context entered (guaranteed by Lock).
    let state = unsafe { bridge::current_state(lock.isolate().as_ffi()) };
    // SAFETY: a non-null pointer is the current context's state, which outlives the call; see
    // above.
    unsafe { state.as_ref() }
}

/// Hands `state` to C++ as the current context's, and returns it, valid as
/// [`current_state()`]'s is.
pub fn install_state<'a>(
    lock: &mut jsg::Lock<'_>,
    state: State,
) -> Result<&'a State, cxx::KjException> {
    // SAFETY: the lock's isolate is locked with a context entered (guaranteed by Lock).
    let state = unsafe { bridge::install_state(lock.isolate().as_ffi(), Box::new(state)) }?;
    // SAFETY: C++ returns the state it now holds, which outlives the call; see `current_state()`.
    Ok(unsafe { &*state })
}

/// Where the zygote pinned the Rust loader's state into the snapshot the isolate starts from, or
/// `None`.
pub fn snapshot_state_index(lock: &mut jsg::Lock<'_>) -> Option<usize> {
    let mut index = 0;
    // SAFETY: the lock's isolate is locked (guaranteed by Lock).
    unsafe { bridge::snapshot_state_index(lock.isolate().as_ffi(), &mut index) }.then_some(index)
}

/// Records where a zygote pinned the Rust loader's state into its snapshot.
pub fn set_snapshot_state_index(lock: &mut jsg::Lock<'_>, index: usize) {
    // SAFETY: the lock's isolate is locked (guaranteed by Lock).
    unsafe { bridge::set_snapshot_state_index(lock.isolate().as_ffi(), index) };
}

/// The script `specifier` of workerd's bundle, or `None` if it has none.
pub fn find_workerd_script<'h>(
    host: &'h bridge::ScriptHost,
    specifier: &str,
) -> Option<&'h bridge::Script> {
    // SAFETY: a non-null script lives as long as the host.
    unsafe { host.find_workerd_script(specifier).as_ref() }
}

/// The script `specifier` of the embedder's bundle, or `None` if it has none or there is no
/// embedder.
pub fn find_embedder_script<'h>(
    host: &'h bridge::ScriptHost,
    specifier: &str,
) -> Option<&'h bridge::Script> {
    // SAFETY: a non-null script lives as long as the host.
    unsafe { host.find_embedder_script(specifier).as_ref() }
}

/// The source of `script` from the host's `jsg::ModuleSource`, or `None` without one.
pub fn module_source<'s>(
    lock: &mut jsg::Lock<'s>,
    host: &bridge::ScriptHost,
    script: &bridge::Script,
) -> Option<v8::Local<'s, v8::String>> {
    // SAFETY: the lock's isolate is locked with a context entered and a HandleScope open
    // (guaranteed by Lock); the result is a handle in that scope.
    unsafe { v8::Local::from_ffi_maybe(host.module_source(lock.isolate().as_ffi(), script)) }
}

/// A new object with the natives of `utils` that forward to the C++ API layer, or `None` with an
/// exception pending.
pub fn api_natives<'s>(
    lock: &mut jsg::Lock<'s>,
    host: &bridge::ScriptHost,
) -> Option<v8::Local<'s, v8::Value>> {
    // SAFETY: the lock's isolate is locked with a context entered and a HandleScope open
    // (guaranteed by Lock); the result is a handle in that scope.
    unsafe { v8::Local::from_ffi_maybe(host.api_natives(lock.isolate().as_ffi())) }
}
