// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Compiling WebAssembly while handling a request, under the `webassembly_dynamic_compilation`
//! compatibility flag. `eval()` stays disallowed, and a copy of each module's bytes goes to the
//! embedder (`IoChannelFactory::submitWasmModule()`) before the module can reach the Worker.

use std::future::Future;

use io_context::IoContext;
use jsg::v8;

use crate::ffi::bridge;

/// The bytes of a WebAssembly module on their way to the embedder, charged to the isolate as
/// external memory until they are dropped, which may be on any thread.
#[derive(Debug, Default)]
pub struct ModuleBytes {
    bytes: Vec<u8>,
    memory: Option<jsg::ExternalMemoryAdjustment>,
}

impl ModuleBytes {
    /// The bytes so far.
    #[must_use]
    pub fn as_slice(&self) -> &[u8] {
        &self.bytes
    }

    /// Adds `bytes`. [`Self::charge`] then charges the isolate for them.
    pub fn append(&mut self, bytes: &[u8]) {
        self.bytes.extend_from_slice(bytes);
    }

    /// Charges the isolate for what the buffer holds.
    pub fn charge(&mut self, lock: &mut jsg::Lock<'_>) {
        let capacity = self.bytes.capacity();
        // The capacity grows geometrically, so this reaches V8 only a few times per module.
        match &mut self.memory {
            Some(memory) if memory.amount() == capacity => {}
            Some(memory) => memory.set(capacity),
            None => self.memory = Some(lock.external_memory_adjustment(capacity)),
        }
    }
}

/// Whether the current request may compile WebAssembly from bytes although `eval()` is
/// disallowed.
///
/// It may when it runs for a Worker with the `webassembly_dynamic_compilation` compatibility
/// flag. Every module such a Worker compiles goes to the embedder first.
pub fn allows_dynamic_compilation(lock: &mut jsg::Lock<'_>) -> bool {
    IoContext::try_current(lock).is_some()
        && lock.feature_flags().get_webassembly_dynamic_compilation()
}

/// Hands `bytes` to the current request's embedder, and returns a future that resolves once the
/// embedder accepts them. The module must not reach the Worker before then.
///
/// # Errors
/// Fails if the embedder refuses the module at once.
pub fn submit(
    bytes: ModuleBytes,
) -> jsg::Result<impl Future<Output = Result<(), cxx::KjException>> + Send + 'static> {
    let submission = bridge::submit_wasm_module(Box::new(bytes))?;
    Ok(crate::OnIoContextThread(bridge::wasm_submission_wait(
        submission,
    )))
}

/// Submits the module bytes in `source`, the first argument of `WebAssembly.compile()`,
/// `instantiate()` or `new WebAssembly.Module()`, if the embedder must see them.
///
/// The embedder must see them if `source` is a non-empty `ArrayBuffer` or typed array, the types
/// V8 compiles from (a `DataView` isn't one), and [`allows_dynamic_compilation`] is true. V8
/// rejects an empty module without compiling anything.
///
/// # Errors
/// Fails if the embedder refuses the module at once.
pub fn submit_source<'s>(
    lock: &mut jsg::Lock<'s>,
    source: v8::Local<'s, v8::Value>,
) -> jsg::Result<Option<impl Future<Output = Result<(), cxx::KjException>> + Send + 'static>> {
    // Checked first so that compiles with nothing to submit allocate nothing.
    if !(source.is_array_buffer() || source.is_typed_array()) || !allows_dynamic_compilation(lock) {
        return Ok(None);
    }
    let bytes = if let Ok(buffer) = v8::Local::<v8::ArrayBuffer>::try_from(source) {
        buffer.to_vec()
    } else {
        v8::Local::<v8::ArrayBufferView>::try_from(source)
            .map_err(|_| jsg::Error::new_internal_error("a typed array is an ArrayBufferView"))?
            .to_vec()
    };
    if bytes.is_empty() {
        return Ok(None);
    }
    let memory = lock.external_memory_adjustment(bytes.capacity());
    submit(ModuleBytes {
        bytes,
        memory: Some(memory),
    })
    .map(Some)
}

/// The isolate's `WasmModuleCallback`, which `new WebAssembly.Module()` calls before it checks or
/// compiles anything. The WebAssembly shim makes sure only a call with `new` reaches it.
///
/// It submits the module like `WebAssembly.compile()`, and the embedder may refuse it at once. The
/// constructor can't wait for the embedder, so it compiles at once, and the submission finishes as
/// a task of the request. If the embedder refuses the module then, the Worker may have kept it
/// anywhere in the isolate, so the isolate is condemned and the request aborted.
///
/// Returns whether it handled the call, which it did only if it threw.
pub fn submit_from_constructor<'s>(
    lock: &mut jsg::Lock<'s>,
    source: v8::Local<'s, v8::Value>,
) -> bool {
    let submitted = submit_source(lock, source).and_then(|submission| {
        if let Some(submission) = submission {
            IoContext::current(lock)?.await_io_and_run(submission, |lock, accepted| {
                let Err(err) = accepted else {
                    return Ok(());
                };
                bridge::abort_isolate_for_refused_module()?;
                let reason = lock.exception(&err.into())?;
                Err(IoContext::current(lock)?.abort(lock, Some(reason)))
            })?;
        }
        Ok(())
    });
    let Err(err) = submitted else {
        return false;
    };
    // V8 then throws what's pending.
    if !lock.is_execution_terminating() {
        lock.throw_exception(&err);
    }
    true
}
