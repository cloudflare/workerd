// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The function through which the WebAssembly shim (`wasm-instantiate-shim.js`) calls V8's
//! `WebAssembly.compile()`, `instantiate()`, `compileStreaming()` and `instantiateStreaming()`.
//!
//! V8 compiles off the main thread and settles their promises from a foreground task, but posting
//! a task doesn't wake the KJ event loop, so nothing would run it until something else did, and
//! then outside the request. While such a promise is pending in a request, the request's
//! [`Pump`] runs V8's foreground tasks inside the request whenever V8 posts one, and once the
//! promise settles, the Worker gets its result there.

use std::cell::Cell;
use std::cell::RefCell;
use std::rc::Rc;

use io_context::IoContext;
use jsg::v8;

use crate::ForegroundTaskSignal;
use crate::dynamic;

/// `compileWasm(original, receiver, args)`, which calls `original` with `receiver` and the
/// arguments in the array `args`, and returns a promise for what V8's promise settles with. A failure
/// rejects the promise.
pub fn new_compile_function<'s>(
    lock: &mut jsg::Lock<'s>,
) -> jsg::Result<v8::Local<'s, v8::Function>> {
    let signal = ForegroundTaskSignal::of(lock);
    v8::Function::new(lock, signal, |lock, signal, info| {
        let (original, receiver, arguments) = (info.get(0), info.get(1), info.get(2));
        let promise =
            match lock.try_catch(|lock| compile(lock, signal, original, receiver, arguments))? {
                Ok(promise) => promise,
                Err(exception) => v8::Promise::rejected(lock, exception)?,
            };
        Ok(promise.into())
    })
}

fn compile<'s>(
    lock: &mut jsg::Lock<'s>,
    signal: &ForegroundTaskSignal,
    original: v8::Local<'s, v8::Value>,
    receiver: v8::Local<'s, v8::Value>,
    arguments: v8::Local<'s, v8::Value>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let original = v8::Local::<v8::Function>::try_from(original)
        .map_err(|_| jsg::Error::new_internal_error("compileWasm() needs the function to call"))?;
    let arguments = v8::Local::<v8::Array>::try_from(arguments)
        .map_err(|_| jsg::Error::new_internal_error("compileWasm() needs an arguments array"))?;
    let length = u32::try_from(arguments.len())
        .map_err(|_| jsg::Error::new_internal_error("compileWasm() got too many arguments"))?;
    let args = (0..length)
        .map(|i| arguments.get(lock, i))
        .collect::<jsg::Result<Vec<_>>>()?;
    let source = args.first().copied().unwrap_or_else(|| lock.undefined());

    // V8 copies the bytes before any JavaScript can run, so it compiles exactly what the embedder
    // gets.
    let submission = dynamic::submit_source(lock, source)?;
    let seen = signal.epoch();
    let result = original.call(lock, Some(receiver), &args)?;
    let promise = v8::Local::<v8::Promise>::try_from(result)
        .map_err(|_| jsg::Error::new_internal_error("V8's compile function returned no promise"))?;

    // Instantiating a compiled Module and a promise V8 already settled (such as a rejection
    // because compiling isn't allowed) don't wait for a foreground task, and compiles at startup
    // have no request to settle in.
    let waits = promise.state() == v8::PromiseState::Pending && !source.is_wasm_module_object();
    let Some(context) = IoContext::try_current(lock).filter(|_| waits || submission.is_some())
    else {
        return Ok(promise);
    };

    let resolver = v8::PromiseResolver::new(lock)?;
    let outer = resolver.promise();
    let waiter = Rc::new(Waiter {
        signal: signal.clone(),
        accepted: Cell::new(submission.is_none()),
        resolver: RefCell::new(Some(resolver.to_global(lock))),
        inner: promise.to_global(lock),
    });
    // The reaction may run in any request or none, so it only wakes the waiter.
    promise.then_or_catch(lock, signal.clone(), on_settled, on_settled)?;

    if let Some(submission) = submission {
        let waiter = Rc::clone(&waiter);
        context.await_io_and_run(submission, move |lock, accepted| {
            match accepted {
                Ok(()) => {
                    waiter.accepted.set(true);
                    waiter.signal.notify();
                }
                // Fails as soon as the embedder rejects the module, without waiting for V8.
                Err(err) => waiter.reject(lock, &jsg::Error::from(err))?,
            }
            Ok(())
        })?;
    }
    Waiter::step(&context, waiter, seen)?;
    Ok(outer)
}

#[expect(
    clippy::unnecessary_wraps,
    reason = "`then_or_catch()` takes fallible reactions"
)]
fn on_settled<'a>(
    _lock: &mut jsg::Lock<'a>,
    signal: &ForegroundTaskSignal,
    value: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    signal.notify();
    Ok(value)
}

/// One compile that waits in a request. The request owns it: each step is an
/// [`IoContext::await_io_and_run`], so if the request goes away first, it is dropped with nothing
/// left waiting.
struct Waiter {
    signal: ForegroundTaskSignal,
    /// Set once the embedder accepted the module, or if it needn't see it.
    accepted: Cell<bool>,
    /// The resolver of the promise the Worker got, until it is settled.
    resolver: RefCell<Option<v8::Global<v8::PromiseResolver>>>,
    /// V8's promise.
    inner: v8::Global<v8::Promise>,
}

impl Waiter {
    /// Waits for V8 to post a task, then settles the Worker's promise if V8's has settled and the
    /// embedder accepted the module, or steps again. `IoContext::run()`, which runs the callback,
    /// runs the foreground tasks after it.
    fn step(context: &IoContext<'_>, waiter: Rc<Self>, seen: u64) -> jsg::Result<()> {
        context.await_io_and_run(waiter.signal.wait_after(seen), move |lock, ()| {
            // Read before the tasks run, so that a task posted while they do wakes the next step.
            let seen = waiter.signal.epoch();
            let Some(resolver) = waiter.resolver.take() else {
                return Ok(());
            };
            let inner = waiter.inner.as_local(lock);
            if waiter.accepted.get() && inner.state() != v8::PromiseState::Pending {
                return resolver.as_local(lock).resolve(lock, inner.into());
            }
            waiter.resolver.replace(Some(resolver));
            Self::step(&IoContext::current(lock)?, waiter, seen)
        })
    }

    fn reject(&self, lock: &mut jsg::Lock<'_>, err: &jsg::Error) -> jsg::Result<()> {
        let Some(resolver) = self.resolver.take() else {
            return Ok(());
        };
        resolver.as_local(lock).reject_with_error(lock, err)
    }
}
