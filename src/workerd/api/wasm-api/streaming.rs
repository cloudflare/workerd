// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `WebAssembly.compileStreaming()` and `instantiateStreaming()`, under the
//! `webassembly_streaming` compatibility flag.
//!
//! The WebAssembly shim checks the Response as the WebAssembly Web API specifies, and hands V8 a
//! function that reads its body in place of the Response. V8 calls the isolate's
//! `WasmStreamingCallback` with that function once it resolves, and [`start`] calls it with a
//! [`WasmStreamingSink`], through which the body reaches V8's streaming compiler.

use std::cell::RefCell;

use io_context::IoContext;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use kj_rs::KjOwn;

use crate::dynamic;
use crate::dynamic::ModuleBytes;
use crate::ffi::bridge;

// SAFETY: Rust uses a `WasmStreaming` only with the isolate lock, apart from dropping it, which
// may happen on any thread: the destructor then posts the abort to the isolate's foreground task
// runner, which is thread-safe.
#[expect(unsafe_code, reason = "used with the isolate lock")]
unsafe impl Send for bridge::WasmStreaming {}

/// The state of one streaming compilation, until it is finished or aborted.
struct Compilation {
    streaming: KjOwn<bridge::WasmStreaming>,
    /// Under `webassembly_dynamic_compilation`, the body so far, which the embedder must accept
    /// before V8 may finish.
    submission: Option<ModuleBytes>,
}

jsg::jsg_traced_unchecked!(Compilation);

/// What the shim's body reader writes to: `push(chunk)` for each `Uint8Array` chunk, then
/// `finish()`, or `abort(reason)`. Dropped without either, it aborts the compilation.
#[jsg_resource(name = "WasmStreamingSink")]
pub struct WasmStreamingSink {
    compilation: RefCell<Option<Compilation>>,
}

#[jsg_resource]
impl WasmStreamingSink {
    #[jsg_method(name = "setUrl")]
    fn set_url(&self, _lock: &mut jsg::Lock<'_>, url: String) {
        if let Some(compilation) = self.compilation.borrow_mut().as_mut() {
            compilation.streaming.pin_mut().set_url(&url);
        }
    }

    #[jsg_method]
    fn push<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        chunk: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        // The shim passes only Uint8Arrays.
        let chunk = v8::Local::<v8::ArrayBufferView>::try_from(chunk)
            .map_err(|_| jsg::Error::new_internal_error("push() needs a Uint8Array"))?;
        let mut compilation = self.compilation.borrow_mut();
        let Some(compilation) = compilation.as_mut() else {
            return Ok(());
        };
        let bytes = chunk.as_slice(lock);
        compilation.streaming.pin_mut().on_bytes_received(bytes);
        if let Some(submission) = compilation.submission.as_mut() {
            submission.append(bytes);
            submission.charge(lock);
        }
        Ok(())
    }

    #[jsg_method]
    fn finish(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<()> {
        let Some(mut compilation) = self.compilation.take() else {
            return Ok(());
        };
        // V8 rejects an empty module without compiling anything, so there's nothing for the
        // embedder then.
        let Some(bytes) = compilation
            .submission
            .take()
            .filter(|bytes| !bytes.as_slice().is_empty())
        else {
            compilation.streaming.pin_mut().finish();
            return Ok(());
        };
        let submission = match dynamic::submit(bytes) {
            Ok(submission) => submission,
            Err(err) => return abort_with_error(lock, compilation, &err),
        };
        IoContext::current(lock)?.await_io_and_run(submission, move |lock, accepted| match accepted
        {
            Ok(()) => {
                compilation.streaming.pin_mut().finish();
                Ok(())
            }
            Err(err) => abort_with_error(lock, compilation, &err.into()),
        })
    }

    #[jsg_method]
    fn abort<'s>(&self, lock: &mut jsg::Lock<'s>, reason: v8::Local<'s, v8::Value>) {
        if let Some(compilation) = self.compilation.take() {
            abort(lock, compilation, reason);
        }
    }
}

/// Takes the lock to show that the isolate is locked.
fn abort<'s>(
    _lock: &jsg::Lock<'s>,
    mut compilation: Compilation,
    reason: v8::Local<'s, v8::Value>,
) {
    // SAFETY: the lock guarantees the isolate is locked, and `reason` is a handle in its scope.
    #[expect(unsafe_code, reason = "hands V8 a handle")]
    unsafe {
        compilation.streaming.pin_mut().abort(reason.into_ffi());
    }
}

/// Aborts with the exception for `err`, which hides an internal error's message. If there is
/// none, because execution is terminating, dropping `compilation` aborts it.
fn abort_with_error(
    lock: &mut jsg::Lock<'_>,
    compilation: Compilation,
    err: &jsg::Error,
) -> jsg::Result<()> {
    let exception = lock.exception(err)?;
    abort(lock, compilation, exception);
    Ok(())
}

/// The isolate's `WasmStreamingCallback`: calls `reader`, the function the shim handed V8, with a
/// sink for `streaming`. V8 calls this as a promise reaction whose result nobody sees, so a
/// failure reaches the compilation through `abort()`.
pub fn start<'s>(
    lock: &mut jsg::Lock<'s>,
    reader: v8::Local<'s, v8::Value>,
    streaming: KjOwn<bridge::WasmStreaming>,
) {
    let submission = dynamic::allows_dynamic_compilation(lock).then(ModuleBytes::default);
    let sink = jsg::Rc::new(
        lock,
        WasmStreamingSink {
            compilation: RefCell::new(Some(Compilation {
                streaming,
                submission,
            })),
        },
    );
    let started = lock.try_catch(|lock| {
        // The shim resolves V8's first argument to the function that reads the body.
        let reader = v8::Local::<v8::Function>::try_from(reader)
            .map_err(|_| jsg::Error::new_internal_error("no body reader"))?;
        let sink = sink.clone(lock).to_js(lock)?;
        reader.call(lock, None, &[sink])?;
        Ok(())
    });
    if let Ok(Err(exception)) = started
        && let Some(compilation) = sink.get(lock).compilation.take()
    {
        abort(lock, compilation, exception);
    }
}
