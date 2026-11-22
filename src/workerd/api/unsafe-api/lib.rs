// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The unsafe modules: `UnsafeEval`, behind `internal:unsafe-eval`, `workerd:unsafe-eval` and the
//! `unsafeEval` binding, `UnsafeModule`, behind `workerd:unsafe`, and `Stdin`, behind
//! `workerd:stdin`. They do what `src/workerd/api/unsafe.c++` does; C++ registers them when the
//! `RUST_UNSAFE_MODULES` autogate is on, and the C++ classes otherwise.

mod ffi;
mod options;
mod stdin;

use std::pin::Pin;

use channel_interop::Fetcher;
use io_context::IoContext;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_traced;

pub use crate::options::EvictOptions;
pub use crate::stdin::Stdin;

/// Compiles and runs code, and compiles WebAssembly, from strings and bytes, which Workers
/// otherwise can't do.
#[jsg_resource(snapshot_stateless)]
#[derive(Default)]
pub struct UnsafeEval;

#[jsg_resource]
impl UnsafeEval {
    /// `eval(script, name = "eval")`: what running `script` as a classic script named `name`
    /// evaluates to.
    #[jsg_method]
    pub fn eval<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        script: String,
        name: Option<String>,
    ) -> jsg::Result<v8::Local<'s, v8::Value>> {
        lock.with_eval_allowed(|lock| {
            let source = new_string(lock, &script)?;
            let name = new_string(lock, name.as_deref().unwrap_or("eval"))?;
            v8::Script::compile(lock, source, name)?.run(lock)
        })
    }

    /// `newFunction(script, name = "anonymous", ...args)`: a function named `name`, with the
    /// parameters `args` and the body `script`.
    #[jsg_method]
    pub fn new_function<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        script: jsg::JsString<'s>,
        name: Option<String>,
        args: jsg::Arguments<jsg::JsString<'s>>,
    ) -> jsg::Result<v8::Local<'s, v8::Function>> {
        lock.with_eval_allowed(|lock| {
            let name = new_string(lock, name.as_deref().unwrap_or("anonymous"))?;
            let params: Vec<v8::Local<'s, v8::String>> = args.into_iter().map(Into::into).collect();
            let function = v8::Function::compile(lock, script.into(), name, &params)?;
            function.set_name(name);
            Ok(function)
        })
    }

    /// `newAsyncFunction(script, name = "anonymous", ...args)`: like `newFunction()`, but an
    /// async function. V8 compiles no async function directly, so this runs the script
    /// `async function {name}({args},) {{script}};{name}`, whose value is the function.
    #[jsg_method]
    pub fn new_async_function<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        script: jsg::JsString<'s>,
        name: Option<String>,
        args: jsg::Arguments<jsg::JsString<'s>>,
    ) -> jsg::Result<v8::Local<'s, v8::Function>> {
        lock.with_eval_allowed(|lock| {
            let name = new_string(lock, name.as_deref().unwrap_or("anonymous"))?;
            let mut prepared = new_string(lock, "async function ")?;
            prepared = concat(lock, prepared, name)?;
            prepared = concat_str(lock, prepared, "(")?;
            for arg in args {
                prepared = concat(lock, prepared, arg.into())?;
                prepared = concat_str(lock, prepared, ",")?;
            }
            prepared = concat_str(lock, prepared, ") {")?;
            prepared = concat(lock, prepared, script.into())?;
            prepared = concat_str(lock, prepared, "}")?;
            prepared = concat_str(lock, prepared, ";")?;
            prepared = concat(lock, prepared, name)?;

            let result = v8::Script::compile(lock, prepared, name)?.run(lock)?;
            if !result.is_async_function() {
                return Err(jsg::Error::new_internal_error(
                    "newAsyncFunction() did not evaluate to an async function",
                ));
            }
            Ok(v8::Local::<v8::Function>::try_from(result)
                .unwrap_or_else(|_| unreachable!("an async function is a function")))
        })
    }

    /// `newWasmModule(src)`: the WebAssembly module that the bytes `src` encode.
    #[jsg_method]
    pub fn new_wasm_module<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        src: jsg::Bytes,
    ) -> jsg::Result<v8::Local<'s, v8::WasmModuleObject>> {
        lock.with_eval_allowed(|lock| v8::WasmModuleObject::compile(lock, &src))
    }
}

impl UnsafeEval {
    /// A new `UnsafeEval`, as the value of an `unsafeEval` binding or a module's exports.
    pub fn new(lock: &mut jsg::Lock<'_>) -> jsg::Rc<Self> {
        jsg::Rc::new(lock, Self)
    }
}

/// Testing controls over the Durable Objects and the process, for tests run in workerd.
#[jsg_resource(snapshot_stateless)]
#[derive(Default)]
pub struct UnsafeModule;

#[jsg_resource]
impl UnsafeModule {
    /// `abortAllDurableObjects()`: aborts every Durable Object of the request's namespaces that
    /// allow it, as if each had thrown `Error: Application called abortAllDurableObjects().`
    #[jsg_method]
    pub fn abort_all_durable_objects<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let io_context = IoContext::current(lock)?;
        io_context.abort_all_actors(application_called("abortAllDurableObjects"))?;
        resolved(lock)
    }

    /// `deleteAllDurableObjects()`: like `abortAllDurableObjects()`, but also deletes their
    /// storage and alarms.
    #[jsg_method]
    pub fn delete_all_durable_objects<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let io_context = IoContext::current(lock)?;
        io_context.delete_all_actors(application_called("deleteAllDurableObjects"))?;
        resolved(lock)
    }

    /// `evict(stub, options)`: evicts the Durable Object behind `stub` from memory, as if it had
    /// been idle, and resolves once it is gone. What fails rejects the promise, as a stub that is
    /// not a Durable Object's, an object that is not running, and invalid `options`.
    #[jsg_method]
    pub fn evict<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        stub: jsg_cpp::Ref<Fetcher>,
        options: Option<EvictOptions>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let io_context = IoContext::current(lock)?;
        let channel = stub.get(lock).subrequest_channel(lock, &io_context)?;
        let eviction = options::web_socket_mode(options).map(|mode| channel.evict_for_test(mode));
        await_eviction(lock, &io_context, eviction)
    }

    /// `evictAllDurableObjects(options)`: like `evict()`, for every Durable Object of the
    /// request's namespaces that allow it.
    #[jsg_method]
    pub fn evict_all_durable_objects<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        options: Option<EvictOptions>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let io_context = IoContext::current(lock)?;
        let eviction = match options::web_socket_mode(options) {
            Ok(mode) => Ok(io_context.evict_all_actors_for_test(mode)?),
            Err(error) => Err(error),
        };
        await_eviction(lock, &io_context, eviction)
    }

    /// `isTestAutogateEnabled()`: whether the `TEST_WORKERD` autogate is on.
    #[jsg_method]
    pub fn is_test_autogate_enabled(&self) -> bool {
        ffi::bridge::is_test_autogate_enabled()
    }

    /// `startedFromStartupSnapshot()`: whether the isolate started from a startup snapshot.
    #[jsg_method]
    pub fn started_from_startup_snapshot(&self, lock: &mut jsg::Lock<'_>) -> bool {
        lock.is_starting_from_snapshot()
    }
}

impl UnsafeModule {
    /// A new `UnsafeModule`, as a module's exports.
    pub fn new(lock: &mut jsg::Lock<'_>) -> jsg::Rc<Self> {
        jsg::Rc::new(lock, Self)
    }
}

/// Registers `internal:unsafe-eval`.
pub fn register_unsafe_modules(registry: Pin<&mut jsg::v8::ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "internal:unsafe-eval",
        |lock| Ok(UnsafeEval::new(lock)),
        jsg::modules::ModuleType::Internal,
    );
}

/// Registers `workerd:unsafe` and `workerd:unsafe-eval`, for Workers with the `unsafe_module`
/// compatibility flag.
pub fn register_unsafe_module(mut registry: Pin<&mut jsg::v8::ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry.as_mut(),
        "workerd:unsafe",
        |lock| Ok(UnsafeModule::new(lock)),
        jsg::modules::ModuleType::Builtin,
    );
    jsg::modules::add_builtin(
        registry,
        "workerd:unsafe-eval",
        |lock| Ok(UnsafeEval::new(lock)),
        jsg::modules::ModuleType::Builtin,
    );
}

/// Registers `workerd:stdin`, which C++ registers only in a build for Fuzzilli.
pub fn register_stdin_module(registry: Pin<&mut jsg::v8::ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "workerd:stdin",
        |lock| Ok(jsg::Rc::new(lock, Stdin)),
        jsg::modules::ModuleType::Builtin,
    );
}

fn new_string<'s>(lock: &mut jsg::Lock<'s>, s: &str) -> jsg::Result<v8::Local<'s, v8::String>> {
    v8::String::new_from_utf8(lock, s.as_bytes()).ok_or_else(invalid_string_length)
}

fn concat<'s>(
    lock: &mut jsg::Lock<'s>,
    left: v8::Local<'s, v8::String>,
    right: v8::Local<'s, v8::String>,
) -> jsg::Result<v8::Local<'s, v8::String>> {
    v8::String::concat(lock, left, right).ok_or_else(invalid_string_length)
}

fn concat_str<'s>(
    lock: &mut jsg::Lock<'s>,
    left: v8::Local<'s, v8::String>,
    right: &str,
) -> jsg::Result<v8::Local<'s, v8::String>> {
    let right = new_string(lock, right)?;
    concat(lock, left, right)
}

/// The error V8 throws for a string longer than it allows.
fn invalid_string_length() -> jsg::Error {
    jsg::Error::new_range_error("Invalid string length")
}

/// The reason with which `abortAllDurableObjects()` and `deleteAllDurableObjects()` abort the
/// objects: what the objects' callers see as `Error: Application called {method}().`
fn application_called(method: &str) -> cxx::KjException {
    use cxx::IntoKjException;
    cxx::KjError::new(
        cxx::KjExceptionType::Failed,
        format!("jsg.Error: Application called {method}()."),
    )
    .into_kj_exception(file!(), line!())
}

/// A promise resolved with `undefined`, followed through a reaction as C++ JSG follows the
/// `jsg::Promise<void>` a method returns.
fn resolved<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let undefined = lock.undefined();
    let promise = v8::Promise::resolved(lock, undefined)?;
    pass_through(lock, promise)
}

/// Waits for `eviction` as part of the request, or rejects with the error that prevented it.
fn await_eviction<'s, F>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    eviction: jsg::Result<F>,
) -> jsg::Result<v8::Local<'s, v8::Promise>>
where
    F: Future<Output = Result<(), cxx::KjException>> + 'static,
{
    let promise = io_context.await_io_local(
        lock,
        async move {
            match eviction {
                Ok(evicted) => evicted.await.map_err(jsg::Error::from),
                Err(error) => Err(error),
            }
        },
        |lock, evicted| {
            evicted?;
            Ok(lock.undefined())
        },
    )?;
    pass_through(lock, promise)
}

/// The data of a reaction that needs none.
#[jsg_traced]
struct NoData;

/// A promise that follows `promise` through a reaction that passes on what it settles with, as
/// C++ JSG follows a promise when it converts it, so that it settles after as many microtasks.
fn pass_through<'s>(
    lock: &mut jsg::Lock<'s>,
    promise: v8::Local<'s, v8::Promise>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    promise.then(lock, NoData, |_lock, _: &NoData, value| Ok(value))
}
